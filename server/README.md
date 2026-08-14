# ApexSim Server

Authoritative racing simulation backend written in Rust. The server owns the 240 Hz physics loop, runs multiplayer race sessions, and distributes telemetry to connected clients. Lobby, session management, and telemetry currently all run over TCP (+TLS); the UDP socket is bound but not yet carrying game traffic. See SPEC.md for the full design (including which parts are implemented vs. planned).

## Repository Layout

```
server/
├── Cargo.toml           # Crate manifest
├── server.toml          # Default runtime configuration (can be overridden)
├── src/
│   ├── main.rs          # Thin binary: CLI parsing, tracing init, bootstrap
│   ├── server.rs        # Server assembly: run_server(), ServerState, 240 Hz game loop
│   ├── config.rs        # TOML config parsing
│   ├── data.rs          # Core data structures (players, cars, tracks, sessions)
│   ├── network.rs       # Message formats shared by TCP/UDP
│   ├── transport.rs     # Async TCP+UDP IO, TLS, heartbeats, routing
│   ├── lobby.rs         # Player lobby management and session discovery
│   ├── game_session.rs  # Session lifecycle + AI helpers
│   ├── physics.rs       # 4-wheel 3D vehicle model + AABB collision detection
│   ├── ai_driver.rs     # AI driver profiles and input generation
│   ├── track_loader.rs  # Track YAML/JSON loading + spline interpolation
│   ├── car_loader.rs    # Car TOML loading
│   ├── replay.rs        # Telemetry recording for race replays
│   ├── health.rs        # HTTP /health and /ready probes
│   └── lib.rs           # Library root (used by the binary and integration tests)
├── benches/             # Criterion benchmarks for the physics hot loop
├── tests/               # Integration tests (spawn the server in-process)
└── SPEC.md              # Product specification (aspirational in parts; see status notes)
```

### Serialization
All network communication between the client and server relies on the [MessagePack](https://msgpack.org/) serialization format via the `rmp_serde` crate, chosen for performance and compact message size. The Godot client implements a matching custom serializer, so wire-format changes must be coordinated with the client.

## Prerequisites

- Rust 1.76+ with `cargo` (tokio + rustls require a modern compiler)
- OpenSSL (only if you plan to generate TLS certs locally)
- Linux or macOS environment; Windows works via WSL2

## Configuration

Runtime settings live in server.toml. Use `cargo run -- --config path/to/custom.toml` to override the defaults. If the config file exists but fails to parse, the server refuses to start; if it is absent, built-in defaults are used.

Key sections:

- `[server]`: `tick_rate_hz` (default 240), `max_sessions`, `session_timeout_seconds`.
- `[network]`: `tcp_bind`, `udp_bind`, and `health_bind` control listener addresses. `tls_cert_path` and `tls_key_path` specify paths to TLS certificate and private key files; leaving both empty means TLS is deliberately off. `require_tls` controls whether TLS is mandatory:
  - When `require_tls = true` (the built-in default): the server fails to start if the certificate paths are empty or the certificates cannot be loaded. Use this for production deployments to prevent accidental plaintext connections.
  - When `require_tls = false`: the server accepts plaintext connections. With empty certificate paths it just logs that TLS is disabled; with paths that are set but unusable it warns, since that is a misconfiguration rather than a choice. The bundled `server.toml` ships this way for development.

  Heartbeat intervals/timeouts are configurable for aggressive or lenient lag handling.
- `[content]`: file system paths for car and track manifests (`cars_dir`, `tracks_dir`). By default the server reuses the repository content tree.
- `[logging]`: `level` accepts `error`, `warn`, `info`, `debug`, `trace`; `console_enabled` toggles console output. You can also override at runtime with `--log-level debug`.
- `[ai]`: default AI driver behavior parameters (optional; defaults apply when the section is omitted).

Generating dev certificates (then point `tls_cert_path`/`tls_key_path` at them):

```
mkdir -p certs
openssl req -x509 -newkey rsa:4096 -keyout certs/server.key -out certs/server.crt \
	-days 365 -nodes -subj "/CN=localhost"
```

## Building

```
cargo build            # Debug build with fast iteration
cargo build --release  # Optimized binary for deployment
cargo fmt && cargo clippy --all-targets   # Lint
```

## Testing

```
cargo test                       # Unit + integration tests (server spawned in-process)
cargo test -- --ignored          # Long-running stress/soak tests
cargo bench                      # Criterion benchmarks for the physics hot loop
```

Integration tests spawn the server in-process on ephemeral ports; no manually started server is required.

For reference, the hot-loop benchmarks land around ~835 ns per car physics step and ~7.8 µs for a complete 8-car session tick (physics + AI + collisions + lap validation) — about 0.2% of the 240 Hz tick budget. If a change moves these numbers materially, that's worth a close look.

## Running the Server

```
# Start with default server.toml
cargo run

# Custom config + verbose logs
cargo run -- --config configs/staging.toml --log-level debug

# Run the compiled binary directly
./target/release/apexsim-server --config /etc/apexsim/server.toml
```

Operational checklist:

1. Ensure car/track assets exist under the configured `content` paths.
2. Configure TLS based on your deployment environment:
   - **Development**: Set `require_tls = false` to allow the server to start without valid certificates. The server will log warnings and accept plaintext connections.
   - **Production**: Set `require_tls = true` and provide valid TLS certificate/key files via `tls_cert_path` and `tls_key_path`. The server will fail to start if certificates are missing or invalid, preventing accidental plaintext deployments.
3. After startup, verify health probes: `curl http://127.0.0.1:9002/health` should return `OK`, while `/ready` flips to `Ready` once content and config are loaded.
4. Check startup logs to confirm the TLS state.
5. Clients authenticate over TCP and currently send `PlayerInput` and receive `Telemetry` over the same TCP connection. Moving high-frequency traffic to UDP (per SPEC.md §3) is planned and will require coordinated client changes.

## Deployment Notes

- The process is a single binary with async tokio runtime; supervise it with systemd or a container orchestrator.
- Use the `/health` and `/ready` HTTP endpoints for liveness/readiness in Kubernetes.
- Set `RUST_LOG=info,apexsim_server=debug` in production to capture session lifecycle events without overwhelming logs.
- Persist replay files by mapping the `replays/` directory to durable storage.

## Further Reading

- SPEC.md: end-to-end architecture, data model, and gameplay rules. Note that parts of the spec (SQLite persistence, race templates/rotation, content hot-reload, Prometheus metrics) are not yet implemented; the spec is being reconciled with reality as the implementation evolves.
