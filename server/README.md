# ApexSim Server

Authoritative racing simulation backend written in Rust. The server owns the 240 Hz physics loop, runs multiplayer race sessions, and distributes telemetry over UDP while keeping the lobby, session management, and persistence layers on the reliable TCP side. This README summarizes how to configure, build, test, and operate the service; refer to SPEC.md plus the docs/ folder for the full design.

## Repository Layout

```
server/
├── Cargo.toml           # Rust workspace manifest for the server crate
├── server.toml          # Default runtime configuration (can be overridden)
├── src/
│   ├── main.rs          # Entry point: config loading, bootstrap, 240 Hz loop
│   ├── config.rs        # TOML config parsing and validation
│   ├── data.rs          # Core data structures (players, cars, tracks, sessions)
│   ├── network.rs       # Message formats shared by TCP/UDP
│   ├── transport.rs     # Async TCP+UDP IO, TLS, heartbeats, routing
│   ├── lobby.rs         # Player lobby management and session discovery
│   ├── game_session.rs  # Session lifecycle + AI helpers
│   ├── physics.rs       # 2D bicycle model + AABB collision detection
│   ├── replay.rs        # Telemetry recording for race replays
│   ├── health.rs        # HTTP /health and /ready probes
│   └── lib.rs           # Shared glue exposed to integration tests
├── tests/
│   └── integration_test.rs  # End-to-end smoke tests
├── docs/                # Design notes (network, implementation, roadmap)
├── SPEC.md              # Full product specification
├── certs/               # TLS assets for TCP (dev self-signed by default)
├── content/             # Local car/track definitions mirrored from content/
└── target/              # Cargo build artifacts (ignored by VCS)
```

## Prerequisites

- Rust 1.76+ with `cargo` (tokio + rustls require a modern compiler)
- OpenSSL (only if you plan to generate TLS certs locally)
- Linux or macOS environment; Windows works via WSL2

## Configuration

Runtime settings live in server.toml. Use `cargo run -- --config path/to/custom.toml` to override the defaults.

Key sections:

- `[network]`: `tcp_bind`, `udp_bind`, and `health_bind` control listener addresses. `tls_cert_path` and `tls_key_path` specify paths to TLS certificate and private key files. `require_tls` controls whether TLS is mandatory:
  - When `require_tls = true`: Server will fail to start if TLS certificates cannot be loaded. Use this for production deployments to prevent accidental plaintext connections.
  - When `require_tls = false` (default): Server logs a warning and accepts plaintext connections if TLS fails to load. Suitable for development environments.
  
  Heartbeat intervals/timeouts are configurable for aggressive or lenient lag handling.
- `[simulation]`: Defines tick rate (default 240 Hz), max players per session, countdown duration, and replay recording switches.
- `[content]`: File system paths for car and track manifests. By default the server reuses the repository content tree; point these settings to production asset buckets when deploying.
- `[logging]`: Accepts `error`, `warn`, `info`, `debug`, `trace`. You can also override at runtime with `--log-level debug`.

Generating dev certificates:

```
mkdir -p certs
openssl req -x509 -newkey rsa:4096 -keyout certs/server.key -out certs/server.crt \
	-days 365 -nodes -subj "/CN=localhost"
```

## Building

```
cargo build            # Debug build with fast iteration
cargo build --release  # Optimized binary for deployment
```

Optional arguments:

- `RUST_LOG=apexsim_server=debug cargo build` to confirm feature flags and dependencies.
- `cargo fmt && cargo clippy --all-targets` for linting.

## Testing

```
cargo test                       # Runs unit + doc tests
cargo test -- --ignored          # Includes ignored stress tests if any
cargo test --test integration_test
```

The integration suite boots the transport layer, exercises basic lobby/session flows, and validates health endpoints. See docs/IMPLEMENTATION.md for the feature checklist covered by automated tests.

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
4. Check startup logs to confirm TLS state: look for "TLS mode: REQUIRED" (encrypted) or "TLS mode: OPTIONAL" (plaintext allowed).
5. Clients authenticate over TCP, send `PlayerInput` over UDP, and receive `Telemetry` at 240 Hz. See SPEC.md §3 for message details.

## Deployment Notes

- The process is a single binary with async tokio runtime; supervise it with systemd or a container orchestrator.
- Use the `/health` and `/ready` HTTP endpoints for liveness/readiness in Kubernetes.
- Set `RUST_LOG=info,apexsim_server=debug` in production to capture session lifecycle events without overwhelming logs.
- Persist replay files and future telemetry databases by mapping the `replays/` and `data/` directories to durable storage.

## Further Reading

- SPEC.md: end-to-end architecture, data model, and gameplay rules
- docs/NETWORK_IMPLEMENTATION.md: deep dive on transport, TLS, and graceful shutdown
- docs/IMPLEMENTATION.md: status tracker for completed modules and testing
- docs/COMPLETE_NETWORK_FEATURES.md: feature-by-feature breakdown of the networking stack

Use these documents when extending or integrating the server; they describe the expected behaviors that accompany this README’s operational guide.