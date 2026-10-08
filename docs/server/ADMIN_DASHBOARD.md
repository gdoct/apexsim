# Server dashboard

The server serves an operator dashboard (design mockup: `server/ApexSim Server Dashboard.html`)
on two ports of its own: HTTP (`admin.http_bind`, 9003) and HTTPS (`admin.https_bind`, 9004).
Both default to loopback. The page, its script, styles and fonts are compiled into the
binary (`server/src/admin/web/`); nothing is served from disk.

## Access
- Everything under `/api` needs the token (`[admin] token`; empty = random per start, printed in the log).
- The login page trades it for an HttpOnly, SameSite=Strict cookie (12 h; `Secure` on HTTPS).
  Scripts may send `Authorization: Bearer <token>`.
- Five wrong tokens from one address lock it out for a minute. Writes with a foreign `Origin` are refused.
- HTTPS uses `admin.tls_*`, else the game port's certificate when its files exist, else a
  self-signed one written next to `bans_file` (`admin-selfsigned.crt/.key`).

## Views (`server/src/admin/api.rs`)
Live (track map + telemetry of any car), Timing (sectors, gaps), Players (kick, ban, unban),
History (recorded sessions from `replays/`, CSV export), Logs (ring buffer fed by a tracing
layer; follow, filter, download), Performance (tick rate/time, CPU, memory, message rates;
1 s samples, 2 min), Config (edit `server.toml`: validated like startup, `.bak` kept, atomic
write), Content (loaded tracks and cars, read-only).

Bans match IP or name at `Authenticate` and persist in `bans_file`. A kick sends an `Error 403`
and closes the connection through the normal disconnect path.

## Not done yet (in the mockup, not built)
- **Schedule** view (a queue of sessions started automatically): needs a scheduler in the game loop.
- **Start / stop / restart** the server and **Save & restart**: the process cannot restart itself; config changes need a manual restart.
- **Broadcast message** to all players (needs a new wire message and a HUD banner).
- **Content upload and enable/disable toggles** (drop a .zip, switch items off): the Content view is read-only.
- **Ping** per player (the server does not measure round trips; Players shows time online and a
  warning tint when a heartbeat is late) and **fastest lap / laps** columns in History (the replay header does not store them).
- Kicking an AI car; live config (everything needs a restart).

Tests: `server/tests/admin_test.rs`. `cargo test --test admin_test serve_demo_dashboard -- --ignored --nocapture`
serves the dashboard on 19003/19004 (token `demo`) over a live AI race for looking at it.
