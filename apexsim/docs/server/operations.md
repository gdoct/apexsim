# Running the server

How to start, configure, secure, watch and deploy `apexsim-server`. The
command line and every `APEXSIM_*` environment variable are listed in
[server/CMDLINE.md](../../server/CMDLINE.md) (shipped in a release as
`Server\server.md`); this page covers the rest.

## Code

- `server/src/main.rs` (CLI, tracing, shutdown), `config.rs`
  (`ServerConfig`, defaults, `validate`, `apply_env_overrides`), `server.rs`
  (`run_server_from`), `health.rs` and `metrics.rs` (health port).
- `server/src/admin/` - the web dashboard (`mod.rs` auth and TLS, `api.rs`
  views, `hosted.rs` sessions the server runs, `schedule.rs`, `content.rs`
  switched-off content and uploads, `bans.rs`, `logbuf.rs`, `sampler.rs`,
  the page in `web/`).
- `sim/src/debug_hooks.rs` - `[debug]` stand-in driver and timed events.
- `server/Dockerfile`, `Dockerfile.content`, `docker-compose.yml`,
  `docker-entrypoint.sh`, `server.docker.toml`; `scripts/deploy_server.ps1`.

## Starting it

```bash
cd server && cargo run --release                     # ./server.toml
apexsim-server --config /etc/apexsim/server.toml --log-level debug
```

Paths in the config (`content`, `showcase`, `records`, TLS files) and the
`./replays` folder resolve against the working directory. A config file that
is absent means built-in defaults (logged); one that is present but does not
parse or validate stops the server. Validation covers the tick rate
(30-1000), bind addresses, that the content folders exist, the log level,
`heartbeat_timeout_ms > heartbeat_interval_ms`, non-zero divisors, and TLS
paths when TLS is required.

Three config files exist, for three places:

| File | Used by | Notes |
|---|---|---|
| `server/server.toml` | `cargo run` in `server/` | Development: 420 Hz, binds 0.0.0.0, TLS off, auth `dev`, content root `../../apexsim-content` |
| `server.toml` (code root) | Copied into the release package's `Server\` | `server/server.toml` with release-relative paths (content root `./content`); keep the two in step |
| `server/server.docker.toml` | The Docker image | Paths under `/content` and `/data`; dashboard on 0.0.0.0 |

All three run at 420 Hz.

## Ports

| Port | Key | What |
|---|---|---|
| 9000/tcp | `[network] tcp_bind` | Login, lobby, sessions, reliable messages |
| 9001/udp | `[network] udp_bind` | Telemetry out, input in, after the UDP handshake |
| 9002/tcp | `[network] health_bind` | `/health`, `/ready`, `/metrics`, `/showcase`, `/recordings` |
| 9003/tcp | `[admin] http_bind` | Dashboard over HTTP (empty: off) |
| 9004/tcp | `[admin] https_bind` | Dashboard over HTTPS (empty: off) |

The built-in defaults bind loopback; to host, bind 9000 and 9001 to
`0.0.0.0` and open 9000/tcp and 9001/udp.

## Configuration

Every section and its keys (defaults from `config.rs`):

- `[server]` - `tick_rate_hz` (420), `max_sessions` (8; a create beyond it is
  refused), `session_timeout_seconds` (300: how long a finished session
  lingers before removal), `reconnect_grace_seconds` (60: how long a session
  whose drivers have all lost their connection waits for one to come back,
  [sessions.md](sessions.md#reconnecting)).
- `[network]` - the three binds, `tls_cert_path` / `tls_key_path`,
  `require_tls` (true), `heartbeat_timeout_ms` (5000), `heartbeat_interval_ms`
  (1000; only validated against the timeout, the server sends nothing on it),
  `telemetry_divisor` (7: telemetry every 7th tick, 60 Hz at 420 Hz).
- `[content]` - `root` (`APEXSIM_CONTENT`; empty is the sibling checkout
  `../../apexsim-content`, as seen from `server/`), whose `cars/` and `tracks/`
  the server reads; `cars_dir`, `tracks_dir` (`APEXSIM_CONTENT_CARS_DIR` /
  `_TRACKS_DIR`) name either folder elsewhere and win over the root. Each is
  read as `default/` then `custom/`; `skip_imported_tracks` (false; the debug test servers set it).
- `[logging]` - `level`, `console_enabled`, `file_enabled` (JSON lines,
  daily rotation, async writer), `file_dir` (`./logs`). `RUST_LOG` wins over
  `--log-level`, which wins over the file.
- `[auth]` - `mode = "dev"` accepts any token (logged as a warning); `mode =
  "token"` accepts only one of `tokens`.
- `[records]` - `enabled`, `dir` (`./records`), `replay_dir` (`./replays`,
  `APEXSIM_REPLAY_DIR`): lap records, ghosts and the
  stored qualifying results ([sessions.md](sessions.md)); `serve_replays`
  (true): the health port hands out the replay folder's recordings and the
  lobby says so (`LobbyState.recordings_port`), for the session browser's
  Recorded list. A `health_bind` on loopback keeps them to this machine.
  `results` (true): each finished race's classification as
  `results/<session>_<unix>.json` and `.csv` under `dir` (`results.rs`,
  `GameSession::classification`): place, driver, car, status, laps, best
  lap, race time, the gap or laps down, the pit rule's penalty.
  `webhook_url` (empty: off) also POSTs each result to an `http://` or
  `https://` endpoint (`webhook.rs`, checked against the built-in Mozilla
  roots, 10 s timeout, never retried); `webhook_format` is `json` (the
  result as written) or `discord` (a `{"content": ...}` message: track,
  distance, a line a car, cut to 2000 characters).
- `[physics]` - `road_contact = "mesh"` (default) or `"centerline"`
  ([apexsim-content/docs/road-mesh.md](../../../apexsim-content/docs/road-mesh.md)).
- `[showcase]` - `enabled`, `dir` (`./showcase`), `playlist`, `mode`
  (`loop` | `rotate`), `stream_divisor` ([../game/spectator.md](../game/spectator.md)).
- `[admin]` - below.
- `[[balance]]` - balance of performance, one table a car (`balance.rs`):
  `car` (its car.toml `name`, case ignored, or its id), `ballast_kg` (0,
  -200 to 300), `power` (1.0, 0.5-1.5: the torque curve scaled, as the
  setup's torque map does) and `downforce` (1.0, 0.5-1.5: both lift
  coefficients, drag left alone). Applied once to the cars as loaded, so
  every session, the AI's plans, the racing line and the setups see the
  balanced car; a car no table names is untouched, and a table naming no
  car is logged. Out-of-range values fail the config check.
- `[debug]` - below.

An `[ai]` table left in an older server.toml is ignored: the AI's driving
comes from each driver's skill ([ai.md](ai.md)).

Environment overrides use the `APEXSIM_` prefix (`APEXSIM_NETWORK_TCP_PORT=9100`,
`APEXSIM_SERVER_TICK_RATE_HZ=120`, `APEXSIM_PHYSICS_ROAD_CONTACT=centerline`...);
a `_PORT` variable replaces only the port of its bind. Each applied override
is logged; one that does not parse is logged and ignored. The full list is in
CMDLINE.md.

## TLS and authentication

- `require_tls = true` (the built-in default) refuses to start without a
  loadable certificate and key. With `require_tls = false`, empty paths mean
  plaintext (logged as info) and set-but-unusable paths fall back to
  plaintext with a warning. Every shipped config file sets it false.
- The game speaks TLS 1.2/1.3 on the TCP port (`FApexTcpConnection`,
  settings.yml `server.tls`, default `auto`: TLS first, plaintext only when
  the server drops the hello unanswered; see
  [../game/client.md](../game/client.md#startup-settings-settingsyml)), so a
  server with TLS on is joined as it is. **UDP is never encrypted**: telemetry
  and input travel in the clear. Every datagram a client sends is sealed
  with a per-connection key that `AuthSuccess` delivered over the (TLS) TCP
  connection and a rising sequence number
  ([protocol.md](protocol.md#transport)), so a forged source address, a
  tampered input or a replay is dropped and counted in
  `apexsim_udp_datagrams_rejected`.
- The server does not make a certificate for the game port (only the
  dashboard makes itself a self-signed one). Make one with
  `openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout certs/server.key -out certs/server.crt -days 365 -subj "/CN=race.example.net" -addext "subjectAltName=DNS:race.example.net,IP:192.168.1.50"`
  (the names and addresses players will type as `host`; rustls reads a PKCS#8,
  PKCS#1 or SEC1 PEM key).

### Running a server players can join securely

1. Certificate. Either a real one for a DNS name (a public CA such as Let's
   Encrypt: players then need nothing but `host`, since the game checks the
   chain against the Windows root store and the host name), or a self-signed
   one as above.
2. `server.toml`: `tls_cert_path` / `tls_key_path` pointing at them and
   `require_tls = true` (the built-in default; every shipped config file
   sets it false for the local, no-certificate case). The server refuses to
   start when it cannot load them, so a broken path is never silently
   plaintext. Switch `[auth]` to `mode = "token"` for a public server.
3. Self-signed: give players the certificate's SHA-256 fingerprint,
   `openssl x509 -in certs/server.crt -noout -fingerprint -sha256`, for
   `server.tls_fingerprint` in their settings.yml (any spelling: colons or
   not, either case). A game that refuses the certificate logs the
   fingerprint it was shown with the line to add; compare it with yours
   rather than copying it blindly, since an attacker's would look the same.
4. Players who want TLS or nothing set `server.tls: on`. In `auto` the game
   falls back to plaintext against a server that does not speak TLS, which
   an attacker on the path can force by resetting the connection; a pin
   turns that off (a pinned server is TLS-only). A changed certificate
   (renewal) changes the fingerprint: hand out the new one.
5. `server.tls_verify: false` encrypts without checking anything: for a
   development box only.

The game logs which it got: `Connected to <host>:<port> over TLS (TLSv1.3,
<cipher>; <how the certificate was trusted>; SHA-256 ...)` or `Connected to
<host>:<port> in PLAINTEXT`.
- Token mode is a shared secret per server, not player accounts. Player
  identity is the name sent at login; records are keyed by it.

## Health and metrics

On `health_bind`:

- `/health` - 200 `OK` while running, 503 once shutdown has begun.
- `/ready` - 200 once content is loaded and the sockets are bound.
- `/metrics` - Prometheus text: `apexsim_connected_players`,
  `apexsim_active_sessions`, `apexsim_telemetry_messages_sent`,
  `apexsim_input_messages_received`, `apexsim_session_tick_panics`,
  `apexsim_tcp_messages_dropped`, `apexsim_udp_messages_dropped`,
  `apexsim_udp_datagrams_rejected` (inbound datagrams the seal refused),
  `apexsim_clients_disconnected_backpressure`, `apexsim_showcase_frames_sent`,
  `apexsim_showcase_viewers`, and the `apexsim_tick_duration_us` histogram.
- `/showcase` - JSON status of the showcase channels.
- `/recordings` - the finished sessions in the replay folder, newest first, at
  most 200, as the dashboard's history lists them (`stream_recorder::history`,
  with each one's `file` and `track_id`); `/recordings/<file>` is that
  stream. Only the recorder's own `replay_*.apxs` names are served
  (`is_served_stream`). Off with `[records] serve_replays = false`.

The loop also warns for every tick over budget and when the rate over 5 s
falls under 90% ([../architecture.md](../architecture.md#tick-timing-on-windows)).

## Admin dashboard

A web page for operators on `[admin] http_bind` / `https_bind` (default
`127.0.0.1:9003` / `:9004`), compiled into the binary from
`server/src/admin/web/`; nothing is served from disk.

- **Access**: everything under `/api` needs `[admin] token` (empty: a random
  one each start, printed once in the log). The login page trades it for an
  HttpOnly, SameSite=Strict cookie lasting 12 h (`Secure` over HTTPS);
  scripts may send `Authorization: Bearer <token>`. Five wrong tokens from one
  address lock it out for a minute; writes with a foreign `Origin` are refused.
  `redirect_http_to_https` sends plain HTTP to the HTTPS listener.
- **HTTPS certificate**: `[admin] tls_cert_path` / `tls_key_path`, else the
  game port's when those files exist, else a self-signed one written beside
  `bans_file` as `admin-selfsigned.crt/.key`.
- **Views** (`api.rs`): overview, live sessions and any car's telemetry,
  timing, sessions (host, start, stop, restart, broadcast; below), schedule
  (below), players (ping, mute, kick, ban, unban, and removing an AI car from its
  session), history (recorded sessions from `replays/`, each with its laps,
  fastest lap and every car's best and laps; CSV export), logs (a ring
  buffer of `log_buffer_lines`, fed by `admin::logbuf::LogBufferLayer`;
  filter, download), performance (1 s samples over 2 minutes), config
  (below), content (below).
- **Hosted sessions** (`hosted.rs`): the Sessions view opens a session the
  server runs, from the same settings a player's create screen has (track,
  the car whose class the AI races in, player slots, AI cars and level,
  laps, a timed race or a weekend, weather, time of day, damage, the pit
  rule, and the race rules: the start, a formation lap, the penalties, the
  safety car and a reverse grid, `HostedSpec.race_rules` as
  `CreateSession.race_rules`). Its host is
  the server (`hosted::SERVER_HOST_ID`, which no connection has), its lobby
  host name the name given; players find it in the session list and join.
  It stays with nobody in it (`GameSession::hosted`,
  `LobbySessionInfo::persistent`) and only the dashboard moves it: START
  (into the countdown, or a weekend's first phase), NEXT PHASE (a weekend
  between phases waits for it), RESTART (ends it and opens it again with
  the same settings, in its lobby) and STOP. START and STOP work on a
  player's session too. STOP sends everyone in the session a `ServerNotice`
  with the reason and a `SessionLeft`. RESTART (`hosted::restart`) sends the
  notice and then seats them in the new session as a join would: each
  connected driver in their car (`handle_join_session`), each spectator
  watching (`handle_join_as_spectator`), with no `SessionLeft` in between;
  a driver whose seat was being held is not carried over. The client takes
  a `SessionJoined` for another session that it did not ask for as that
  move (`UApexNetSubsystem`: back to the lobby state, a spectator if seated
  at grid position 0). A broadcast sends a `ServerNotice` to everyone
  connected or to one session.
- **Schedule** (`schedule.rs`, kept in `[admin] schedule_file`): an entry is
  a hosted session's settings, a start time, how many minutes before it the
  session opens in the lobby, and once, daily or weekly. A task checks it
  every second: it opens the session at the open time, starts it at the
  start time and moves the entry on to its next time (a one-off is done),
  and while that session runs it moves a weekend on as each phase ends
  (unless the entry says to wait for NEXT PHASE). An occurrence more than 10
  minutes late to open (the server was down) is skipped and noted; one ended
  before its start counts as cancelled. Times are Unix seconds, so a daily
  entry keeps to UTC across a change of the clocks; the page shows them in
  the browser's time.
- **Content** (`content.rs`): every car and track can be switched off (kept
  in `[admin] content_file`): the lobby leaves it out, a select or create
  naming it is refused with `Error 403`, and the AI field is never dealt it
  (`GameSession::ai_excluded_cars`); a session already on it runs on.
  Uploads go one file per request into `custom/` and load at once: a
  `car.toml` to `cars/custom/<car name>/`, a track's YAML and its sidecars
  to `tracks/custom/<Stem>/` (sidecars first: the YAML puts it in the
  catalog, and a re-uploaded sidecar is read again by the next session on
  the track). A file the server cannot parse is refused before anything is
  replaced, and an upload never takes a loaded car's or track's id unless it
  is the same custom file again. Players need the same files to race on
  them (the client renders from its own content), and the content checksum
  tells them when theirs differ.
- **Config** edits the `server.toml` the process was started from:
  validated as at startup, `.bak` kept, atomic write. The keys in
  `config::LIVE_KEYS` apply as it is saved (`[server]` max_sessions,
  session_timeout_seconds, reconnect_grace_seconds; `[auth]`, read at each
  `Authenticate`; `[logging] level`, unless `RUST_LOG` or `--log-level`
  set it; `[admin] log_buffer_lines`; `[debug]`, from the next session); an
  `APEXSIM_` variable still wins. The save answers which keys applied and
  which wait for a restart.
- **Restart** (RESTART SERVER on the Config view, `POST /api/server/restart`)
  stops the process and brings it back on the saved file, for the keys a
  save cannot apply. What happens is `[admin] restart`: `respawn` (the
  default) starts a copy of the process with the same command line and
  environment, told through `APEXSIM_RESTART_WAIT` to wait (up to 30 s)
  for the game ports to come free, then shuts this one down as a signal
  would (every player gets the shutdown notice and is dropped); `exit`
  only shuts down, for a supervisor that starts it again (the Docker image
  sets it: `restart: unless-stopped` in the compose file); `off` refuses
  with 409 and hides the button. The page polls `/api/overview` until the
  API answers again and reloads. A copy started by `respawn` inherits the
  console and the standard streams, so a script that waits on the first
  process's output keeps waiting on the copy's.
- **Bans** match IP or name at `Authenticate` and persist in `bans_file`. A
  kick sends `Error 403` and closes the connection through the normal
  disconnect path (`TransportLayer::kick_player`).
- **Mute** (`POST /api/players/<id>/mute`, `/unmute`; the MUTE / UNMUTE
  button) stops a player's chat lines (`crate::chat::ChatGuard`): they get
  `Error 403` and nobody hears them. Kept for the server's life, across a
  reconnect, not on disk.

- **Ping** is the game's own measure of its heartbeat round trip,
  reported in the next `Heartbeat` (`ping_ms`); a client that does not
  report one shows none.

Tests: `server/tests/all/admin_test.rs` (`a_session_is_run_from_the_dashboard_alone`
is the end-to-end check). `cargo test --test all
admin_test::serve_demo_dashboard -- --ignored --nocapture` serves it on 19003/19004
(token `demo`) over a live AI race.

## Docker

`server/Dockerfile` builds both binaries into a slim Debian image running as
user `apexsim`, working directory and volume `/data`, ports 9000-9004, a
`HEALTHCHECK` on `/health`. Both images are built from the folder that holds
the roots, each filtered by its own `<Dockerfile>.dockerignore`: the server
builds against `apexsim/sim` and `apexsim-lib/formats` (`docker build -f
apexsim/server/Dockerfile -t apexsim-server .`). Content is not baked in:
`Dockerfile.content` (`docker build -f
apexsim/server/Dockerfile.content -t apexsim-content .`) packs
`apexsim-content/cars`, `apexsim-content/tracks` (with the generated sidecars)
and `apexsim/build/showcase`, and on start replaces the `/content` volume
with them. `docker-compose.yml` runs the content job, then the server.
`docker-entrypoint.sh` seeds `/data/server.toml` from the image on first start
so the dashboard's edits survive a new container. `server.docker.toml` has
TLS off and auth `dev`: fine on a LAN, not on the internet.

`scripts/deploy_server.ps1` deploys both images to a remote Docker host over
ssh from `deploy/deploy.user.psd1` (gitignored; start from
`deploy/deploy.sample.psd1`), rendering `docker-compose.yml` and
`server.toml` into `build/deploy/`. `-Action deploy|status|logs|restart|stop`,
`-SkipBuild`, `-SkipServer`, `-SkipContent`, `-DryRun`.

## Files the server writes

`[records] dir`: `lap_records.json`, `ghosts/*.msgpack`, `qualifying.json`,
`results/*.json` and `*.csv` (a finished race's classification),
the watched hotlaps' `watch_levels.json` and `watch_ghosts/*.msgpack`,
and by default the dashboard's `bans.json`, `content.json` (switched-off
content) and `schedule.json` (a corrupt records file is logged and treated
as empty). `./replays/`: a replay (`.apxs` plus a `.meta.json`) per finished race,
streamed to `.recording_<session>.blocks` while racing. `[logging] file_dir` when file
logging is on.

## Shutdown

SIGINT or SIGTERM marks `/health` unhealthy, sends every client `Error 503
"Server is shutting down"`, waits 500 ms and exits.

## Debug hooks

`[debug]` (`APEXSIM_DEBUG_STAND_IN_DRIVER`, `APEXSIM_DEBUG_EVENTS`): never on
a server people race on. `stand_in_driver = true` has an AI drive every
human's car in every mode (race, practice, qualifying runs and shoot-outs
once the car is out of its garage), with the steering aid off whatever the
driver's profile says (`hand_cars_to_stand_in`, each tick); `events = "<s of green>:<host|all|car index>:<action>;..."`
fires `wear=P`, `puncture=FL`, `leak=FL:KPA`, `flatspot=FL:S`,
`heat=FL:C` (a tyre's tread and core at C °C; `front`, `rear` or `all` for
several),
`damage=<zone>:P`, `brakewear=P`, `pit`, `boost=S`, `aids=off` or
`pitlane=manual` (the pit autopilot off) at a time
of green. Not applied to demo sessions. Tests: `sim/tests/all/debug_hooks_test.rs`.
`tick_jitter_seconds = N` (`APEXSIM_DEBUG_TICK_JITTER_SECONDS`) logs one
line per N seconds of how evenly the game loop ticked (`tick_jitter.rs`):
the tick-to-tick interval's median, 99th percentile and longest, and its
spread against the tick's budget. Measure on an idle machine.

## Traps

- A plaintext server logs `Message too large ... 369295617 bytes` once per
  game connection in `server.tls: auto`: that is the game's TLS hello read as
  a frame length, before it reconnects in plaintext. Harmless.
- A self-signed certificate without a matching `tls_fingerprint` is refused
  by the game and it does not retry (the same certificate would fail again).
- The dashboard's Config view applies only `config::LIVE_KEYS`; a binding,
  the tick rate, TLS, content folders and the rest need a restart (the save
  says which; RESTART SERVER does it).
- A `respawn` restart whose copy cannot bind in 30 s starts anyway and
  fails on the taken port like any server: the old process is then gone and
  nothing listens. Something else holding the port is the only way there.
- A hosted session never ends by itself in its lobby: STOP it, or let the
  schedule start it. A finished one is removed after
  `session_timeout_seconds` as any is.
