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
  views, `bans.rs`, `logbuf.rs`, `sampler.rs`, the page in `web/`).
- `server/src/debug_hooks.rs` - `[debug]` stand-in driver and timed events.
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
| `server/server.toml` | `cargo run` in `server/` | Development: 420 Hz, binds 0.0.0.0, TLS off, auth `dev`, content `../content` |
| `server.toml` (repo root) | Copied into the release package's `Server\` | `server/server.toml` with release-relative paths (`./content`); keep the two in step |
| `server/server.docker.toml` | The Docker image | Paths under `/content` and `/data`; dashboard on 0.0.0.0 |

All three run at 420 Hz.

## Ports

| Port | Key | What |
|---|---|---|
| 9000/tcp | `[network] tcp_bind` | Login, lobby, sessions, reliable messages |
| 9001/udp | `[network] udp_bind` | Telemetry out, input in, after the UDP handshake |
| 9002/tcp | `[network] health_bind` | `/health`, `/ready`, `/metrics`, `/showcase` |
| 9003/tcp | `[admin] http_bind` | Dashboard over HTTP (empty: off) |
| 9004/tcp | `[admin] https_bind` | Dashboard over HTTPS (empty: off) |

The built-in defaults bind loopback; to host, bind 9000 and 9001 to
`0.0.0.0` and open 9000/tcp and 9001/udp.

## Configuration

Every section and its keys (defaults from `config.rs`):

- `[server]` - `tick_rate_hz` (420), `max_sessions` (8; a create beyond it is
  refused), `session_timeout_seconds` (300: how long a finished session
  lingers before removal).
- `[network]` - the three binds, `tls_cert_path` / `tls_key_path`,
  `require_tls` (true), `heartbeat_timeout_ms` (5000), `heartbeat_interval_ms`
  (1000; only validated against the timeout, the server sends nothing on it),
  `telemetry_divisor` (7: telemetry every 7th tick, 60 Hz at 420 Hz).
- `[content]` - `cars_dir`, `tracks_dir` (each read as `default/` then
  `custom/`), `skip_imported_tracks` (false; the debug test servers set it).
- `[logging]` - `level`, `console_enabled`, `file_enabled` (JSON lines,
  daily rotation, async writer), `file_dir` (`./logs`). `RUST_LOG` wins over
  `--log-level`, which wins over the file.
- `[auth]` - `mode = "dev"` accepts any token (logged as a warning); `mode =
  "token"` accepts only one of `tokens`.
- `[records]` - `enabled`, `dir` (`./records`): lap records, ghosts and the
  stored qualifying results ([sessions.md](sessions.md)).
- `[physics]` - `road_contact = "mesh"` (default) or `"centerline"`
  ([../content/road-mesh.md](../content/road-mesh.md)).
- `[showcase]` - `enabled`, `dir` (`./showcase`), `playlist`, `mode`
  (`loop` | `rotate`), `stream_divisor` ([../game/spectator.md](../game/spectator.md)).
- `[admin]` - below.
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
  and input travel in the clear, and a UDP sender's identity is the address
  bound by its handshake with the token that `AuthSuccess` delivered over the
  (TLS) TCP connection.
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
  `apexsim_clients_disconnected_backpressure`, `apexsim_showcase_frames_sent`,
  `apexsim_showcase_viewers`, and the `apexsim_tick_duration_us` histogram.
- `/showcase` - JSON status of the showcase channels.

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
  timing, players (kick, ban, unban), history (recorded sessions from
  `replays/`), logs (a ring buffer of `log_buffer_lines`, fed by
  `admin::logbuf::LogBufferLayer`; filter, download), performance (1 s
  samples over 2 minutes), config (edits the `server.toml` the process was
  started from: validated as at startup, `.bak` kept, atomic write; takes
  effect at the next start), content (read-only).
- **Bans** match IP or name at `Authenticate` and persist in `bans_file`. A
  kick sends `Error 403` and closes the connection through the normal
  disconnect path (`TransportLayer::kick_player`).

Tests: `server/tests/admin_test.rs`. `cargo test --test admin_test
serve_demo_dashboard -- --ignored --nocapture` serves it on 19003/19004
(token `demo`) over a live AI race.

## Docker

`server/Dockerfile` builds both binaries into a slim Debian image running as
user `apexsim`, working directory and volume `/data`, ports 9000-9004, a
`HEALTHCHECK` on `/health`. Content is not baked in: `Dockerfile.content`
(built from the repo root, `docker build -f server/Dockerfile.content -t
apexsim-content .`) packs `content/cars`, `content/tracks` (with the generated
sidecars) and `build/showcase`, and on start replaces the `/content` volume
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

`[records] dir`: `lap_records.json`, `ghosts/*.msgpack`, `qualifying.json`
and by default the dashboard's `bans.json` (a corrupt records file is logged
and treated as empty). `./replays/`: a replay per finished race, streamed to
`.recording_<session>.frames` while racing. `[logging] file_dir` when file
logging is on.

## Shutdown

SIGINT or SIGTERM marks `/health` unhealthy, sends every client `Error 503
"Server is shutting down"`, waits 500 ms and exits.

## Debug hooks

`[debug]` (`APEXSIM_DEBUG_STAND_IN_DRIVER`, `APEXSIM_DEBUG_EVENTS`): never on
a server people race on. `stand_in_driver = true` has an AI drive every
human's car; `events = "<s of green>:<host|all|car index>:<action>;..."`
fires `wear=P`, `puncture=FL`, `leak=FL:KPA`, `flatspot=FL:S`,
`damage=<zone>:P`, `brakewear=P`, `pit`, `boost=S` or `aids=off` at a time
of green. Not applied to demo sessions. Tests: `tests/debug_hooks_test.rs`.

## Traps

- A plaintext server logs `Message too large ... 369295617 bytes` once per
  game connection in `server.tls: auto`: that is the game's TLS hello read as
  a frame length, before it reconnects in plaintext. Harmless.
- A self-signed certificate without a matching `tls_fingerprint` is refused
  by the game and it does not retry (the same certificate would fail again).
- The dashboard's Config view reloads nothing: every change needs a restart.
