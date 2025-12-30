# Network Layer, Health Endpoint, and Encryption Implementation

## Summary

This document describes the implementation of the network layer, health check endpoints, and TLS encryption for the ApexSim Racing Server.

## Implemented Features

### 1. Network Transport Layer (`src/transport.rs`)

A complete async TCP/UDP transport layer with the following capabilities:

- **TCP Communication**:
  - Async TCP listener on configurable bind address
  - Per-connection bidirectional channels for message passing
  - Split reader/writer tasks for each connection
  - Length-prefixed message framing (4-byte big-endian length + bincode payload)
  - Connection tracking with player ID mapping
  - Automatic authentication handling

- **UDP Communication**:
  - High-frequency unreliable message transport (ideal for telemetry at 240Hz)
  - Separate sender and receiver tasks
  - Address-based connection correlation

- **TLS Encryption**:
  - TLS 1.3 support via `rustls` and `tokio-rustls`
  - Self-signed certificate support for development
  - Graceful fallback if certificates are not found (with warning)
  - Certificate loading from configurable paths

- **Connection Management**:
  - Connection ID derivation from socket address hashing
  - Player-to-connection mapping for routing
  - Heartbeat timeout tracking
  - Automatic stale connection cleanup

### 2. Health Check Endpoint (`src/health.rs`)

HTTP-based health monitoring with two endpoints:

- **`/health`**: Returns 200 OK if server is accepting connections, 503 during shutdown
- **`/ready`**: Returns 200 OK if content is loaded and database is connected, 503 otherwise

Features:
- Separate HTTP server on configurable port (default: 9002)
- Async request handling with Hyper 1.0
- Shared health state with atomic updates
- Suitable for Kubernetes liveness and readiness probes

### 3. Configuration Updates

Enhanced `server.toml` and `ServerConfig` with:

```toml
[network]
tcp_bind = "127.0.0.1:9000"
udp_bind = "127.0.0.1:9001"
health_bind = "127.0.0.1:9002"
tls_cert_path = "./certs/server.crt"
tls_key_path = "./certs/server.key"
heartbeat_interval_ms = 1000
heartbeat_timeout_ms = 5000
```

### 4. Main Server Integration

Updated [main.rs](src/main.rs) to:
- Initialize health state before starting services
- Start health check HTTP server in background task
- Initialize and start transport layer
- Mark server as ready after successful initialization
- Pass transport layer to game loop for future integration

## Architecture

### Connection Flow

1. **TCP Connection**:
   ```
   Client → TCP Connect → TLS Handshake (optional) → Authenticate Message
   Server → Assign Player ID → Create per-connection channel → Send AuthSuccess
   Client ↔ Server → Bidirectional message exchange via dedicated channels
   ```

2. **UDP Communication**:
   ```
   Client → Send PlayerInput @ 240Hz (unreliable)
   Server → Send Telemetry @ 240Hz (unreliable)
   ```

3. **Health Checks**:
   ```
   Monitoring System → HTTP GET /health → 200 OK / 503 Unavailable
   Monitoring System → HTTP GET /ready → 200 OK / 503 Not Ready
   ```

### Message Serialization

- Protocol: Binary serialization via `bincode`
- TCP Framing: `[4-byte length][bincode payload]`
- UDP: Raw bincode (no framing, single datagram per message)

### TLS Configuration

Self-signed certificates can be generated with:
```bash
openssl req -x509 -newkey rsa:4096 -keyout server.key -out server.crt \
  -days 365 -nodes -subj "/CN=localhost"
```

Place in `./certs/` directory or configure custom paths in `server.toml`.

## Testing

All tests pass (26 tests total):
```bash
cargo test --lib
```

Server can be started with:
```bash
cargo run
```

Test endpoints:
```bash
curl http://127.0.0.1:9002/health  # Should return "OK"
curl http://127.0.0.1:9002/ready   # Should return "Ready"
```

## Dependencies Added

```toml
rustls = "0.23"
tokio-rustls = "0.26"
rustls-pemfile = "2"
hyper = { version = "1", features = ["full"] }
hyper-util = { version = "0.1", features = ["full"] }
http-body-util = "0.1"
bytes = "1"
```

## Current Status

✅ Network layer fully implemented and tested
✅ Health endpoints operational
✅ TLS encryption support (optional)
✅ Connection tracking and management
✅ Per-connection message channels
✅ Zero warnings on compilation
✅ All existing tests still pass

## Completed Enhancements (Latest Update)

### Full TLS Support
✅ **Implemented**: TLS 1.3 stream handling with generic async stream support
- Proper TLS handshake using `rustls` and `tokio-rustls`
- Graceful fallback to unencrypted mode if TLS not configured
- Both TLS and non-TLS streams handled through same code path
- Length-prefixed message framing for reliable message boundaries

### Message Routing
✅ **Implemented**: Full integration between transport layer and game loop
- Non-blocking message processing in game loop (100μs timeout)
- Player input routing to physics simulation
- Session creation/join handling
- Real-time input processing at 240Hz

### Graceful Shutdown
✅ **Implemented**: Clean shutdown with client notifications
- Broadcasts error message to all connected clients (code 503)
- 500ms grace period for messages to send
- Health endpoint marks server as unhealthy
- Connection cleanup before exit

### Heartbeat Handling
✅ **Implemented**: Automatic heartbeat processing
- Heartbeat messages update connection last-seen time
- Automatic cleanup of stale connections (every second)
- Heartbeat ACK responses with server tick
- Configurable timeout (default: 5 seconds)

## Advanced Session Management (Completed)

✅ **Lobby System** ([src/lobby.rs](src/lobby.rs))
- Full lobby management for players to browse and join sessions
- Session visibility controls (Public/Private/Protected)
- Player tracking and session assignment
- Join as participant or spectator
- Session state broadcasting support

✅ **Spectator Mode** ([src/lobby.rs](src/lobby.rs))
- Read-only session participation
- Separate spectator tracking from active players
- Spectator count management
- Join ongoing sessions as observer

✅ **Session Replay System** ([src/replay.rs](src/replay.rs))
- Frame-by-frame replay recording during sessions
- Binary file format with metadata header
- Replay playback with seek/reset controls
- Automatic session recording management
- Replay metadata (participants, track, duration)

## Lobby Message Handlers (Completed)

✅ **Full Lobby Integration** ([src/main.rs](src/main.rs))
- **Authentication**: Players automatically added to lobby on auth success
- **SelectCar**: Update player's selected car in lobby
- **RequestLobbyState**: Send current lobby state to requesting client
- **CreateSession**: Create session and register with lobby manager
- **JoinSession**: Join existing session as participant with validation
- **JoinAsSpectator**: Join session in spectator mode
- **LeaveSession**: Leave session and return to lobby
- **Disconnect**: Clean removal from lobby on disconnect
- **Periodic Broadcast**: Lobby state broadcast every 2 seconds to all clients

## Client Messages Added

New client messages for lobby operations:
- `RequestLobbyState` - Request current lobby state
- `JoinAsSpectator { session_id }` - Join session as spectator

## Replay Integration (Completed)

✅ **Automatic Replay Recording** ([src/main.rs](src/main.rs:510-608))
- **Auto-start**: Recording begins when session transitions to Racing state
- **Frame Recording**: Telemetry captured every tick (240Hz) during racing
- **Auto-save**: Replay automatically saved when session finishes
- **Binary Format**: Efficient bincode serialization to `./replays/` directory
- **Metadata**: Includes session ID, track, participants, timestamp
- **State Management**: Borrow-checker safe deferred execution pattern

Recording lifecycle:
1. Session enters Racing → Start recording with metadata
2. Each tick while Racing → Record telemetry frame
3. Session finishes → Stop and save replay to disk

## Future Enhancements

The following features require additional dependencies and are planned for future implementation:

1. **DTLS for UDP**: Add encrypted UDP support using DTLS 1.3 for production deployments.
   - Requires: `tokio-dtls` or `rustls-dtls` crates
   - Implementation: Similar pattern to TLS, negotiate DTLS session per client
   - Benefit: Encrypted telemetry at 240Hz

2. **Prometheus Metrics**: Add `/metrics` endpoint for monitoring:
   - Requires: `prometheus` crate
   - Metrics to track:
     - Connection counts (gauge)
     - Message throughput (counter)
     - Latency histograms
     - Error rates (counter)
     - Active sessions (gauge)
   - Integration: Add metrics collection in transport and game loop

3. **Rate Limiting**: Implement per-connection rate limiting for DoS protection.
   - Requires: `governor` or `ratelimit` crate
   - Strategy: Token bucket per connection
   - Limits: Configurable per message type (e.g., 10 CreateSession/min, 300 PlayerInput/sec)
   - Action: Return error 429 (Too Many Requests) when exceeded

4. **Replay HTTP API**: Add HTTP endpoints for replay management:
   - `GET /api/replays` - List available replays
   - `GET /api/replays/{id}` - Download specific replay
   - `POST /api/replays/{id}/playback` - Start playback session
   - Requires: Extend health HTTP server with additional routes

## Notes

- The transport layer is designed to be independent of game logic
- All network I/O is fully async using Tokio
- Connection state is protected by RwLocks for safe concurrent access
- The architecture supports thousands of concurrent connections
- Health endpoints are ready for production monitoring systems
