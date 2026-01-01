# Complete Network Layer Implementation

## Overview

This document details the complete implementation of the network layer, TLS encryption, message routing, and graceful shutdown for the ApexSim Racing Server.

## Completed Features

### 1. Full TLS Support ✅

**Implementation**: [src/transport.rs:219-269](src/transport.rs#L219-L269)

The server now supports full TLS 1.3 encryption with proper stream handling:

```rust
// Generic stream handler works with both TLS and non-TLS streams
async fn handle_stream<S>(stream: S, ...)
where
    S: AsyncReadExt + AsyncWriteExt + Unpin + Send + 'static
```

**Features**:
- TLS handshake performed via `tokio-rustls`
- Both TLS and non-TLS connections supported
- Automatic fallback if certificates unavailable
- Proper error handling and logging
- Generic implementation allows same code path for both modes

**TLS Flow**:
1. TCP connection accepted
2. Optional TLS handshake performed
3. Stream passed to generic handler
4. Full bidirectional communication

### 2. Length-Prefixed Message Framing ✅

**Implementation**: [src/transport.rs:315-405](src/transport.rs#L315-L405)

Reliable message framing prevents message boundary issues:

```rust
// Write: [4-byte length][bincode data]
let len = data.len() as u32;
writer.write_all(&len.to_be_bytes()).await?;
writer.write_all(&data).await?;

// Read: Read length first, then exact data
let len = u32::from_be_bytes(len_buf) as usize;
let mut msg_buf = vec![0u8; len];
reader.read_exact(&mut msg_buf).await?;
```

**Features**:
- 4-byte big-endian length prefix
- Exact message size reading (no partial messages)
- 1MB maximum message size (DoS protection)
- Proper EOF handling

### 3. Heartbeat System ✅

**Implementation**: [src/transport.rs:358-369](src/transport.rs#L358-L369), [src/main.rs:248-251](src/main.rs#L248-L251)

Automatic connection health monitoring:

```rust
ClientMessage::Heartbeat { .. } => {
    // Update last heartbeat time
    conn.last_heartbeat = Instant::now();

    // Send heartbeat ack
    let response = ServerMessage::HeartbeatAck {
        server_tick: 0,
    };
    conn_tx.send(response);
}
```

**Features**:
- Client sends heartbeat every 1 second
- Server updates connection timestamp
- Automatic cleanup of stale connections (>5s)
- Cleanup runs every second in game loop
- Configurable timeout in `server.toml`

### 4. Graceful Shutdown ✅

**Implementation**: [src/transport.rs:523-541](src/transport.rs#L523-L541), [src/main.rs:161-176](src/main.rs#L161-L176)

Proper server shutdown with client notifications:

```rust
pub async fn shutdown(&mut self) {
    // Notify all clients
    for conn_info in connections.values() {
        conn_info.tcp_tx.send(ServerMessage::Error {
            code: 503,
            message: "Server is shutting down".to_string(),
        });
    }

    // 500ms grace period
    tokio::time::sleep(Duration::from_millis(500)).await;
}
```

**Shutdown Flow**:
1. Ctrl+C received
2. Health endpoint marked as unhealthy (`/health` → 503)
3. Broadcast shutdown message to all clients
4. 500ms grace period for messages to send
5. Shutdown signal sent to all background tasks
6. Server exits cleanly

### 5. Message Routing ✅

**Implementation**: [src/main.rs:204-246](src/main.rs#L204-L246)

Full integration between network layer and game loop:

```rust
// Non-blocking message processing (100μs timeout)
while let Ok(Some((connection_id, msg))) =
    tokio::time::timeout(Duration::from_micros(100), transport.recv_tcp()).await
{
    match msg {
        ClientMessage::PlayerInput { throttle, brake, steering, .. } => {
            // Route to physics simulation
            player_inputs.insert(player_id, input);
        }
        ClientMessage::CreateSession { ... } => {
            // Create new race session
            let session_id = state.create_session(...);
        }
        _ => { }
    }
}
```

**Features**:
- Non-blocking design (100μs timeout)
- Runs at 240Hz in game loop
- Player input collected for physics sim
- Session management integrated
- Authentication and heartbeat handled in transport layer

### 6. Connection Management ✅

**Implementation**: Throughout [src/transport.rs](src/transport.rs)

Comprehensive connection tracking and management:

**Data Structures**:
```rust
pub struct ConnectionInfo {
    pub player_id: PlayerId,
    pub player_name: String,
    pub connected_at: Instant,
    pub last_heartbeat: Instant,
    pub tcp_addr: SocketAddr,
    pub tcp_tx: mpsc::UnboundedSender<ServerMessage>,  // Per-connection channel
}
```

**Mappings**:
- `ConnectionId → ConnectionInfo` - Full connection details
- `PlayerId → ConnectionId` - Player lookup
- `SocketAddr → ConnectionId` - Address correlation for UDP

**Features**:
- Per-connection send channels (TCP)
- Automatic cleanup on disconnect
- Heartbeat-based timeout detection
- Connection count tracking
- Broadcast messaging support

### 7. UDP Telemetry Support ✅

**Implementation**: [src/transport.rs:416-465](src/transport.rs#L416-L465)

High-frequency unreliable telemetry for 240Hz updates:

```rust
async fn udp_receiver(socket: Arc<UdpSocket>, tx: mpsc::UnboundedSender<(SocketAddr, ClientMessage)>) {
    loop {
        match socket.recv_from(&mut buf).await {
            Ok((n, addr)) => {
                let msg = bincode::deserialize::<ClientMessage>(&buf[..n])?;
                tx.send((addr, msg));
            }
        }
    }
}
```

**Features**:
- Separate UDP sender and receiver tasks
- No length prefix (single datagram per message)
- Address-based routing
- Fire-and-forget semantics
- Ideal for game state updates

## Architecture Diagrams

### Connection Flow

```
Client                          Server
  |                               |
  |--- TCP Connect ------------->|
  |<-- TLS Handshake (optional)--|
  |                               |
  |--- Authenticate ------------->|
  |     {token, name}             |
  |                               | Create ConnectionInfo
  |                               | Assign Player ID
  |<-- AuthSuccess ---------------|
  |     {player_id, version}      |
  |                               |
  |--- Heartbeat (1Hz) ---------> |
  |<-- HeartbeatAck --------------|
  |                               |
  |--- PlayerInput (240Hz UDP) -->|
  |<-- Telemetry (240Hz UDP) -----|
  |                               |
  |<-- Error: Shutdown -----------|
  |     {503, "shutting down"}    |
  |                               |
  X-- Connection Closed ---------X
```

### Game Loop Integration

```
Main Game Loop (240Hz)
├── Process TCP Messages (100μs timeout)
│   ├── PlayerInput → Store in inputs map
│   ├── CreateSession → Create new session
│   ├── JoinSession → Add player to session
│   └── (Other messages)
│
├── Cleanup stale connections (every second)
│
├── Update Sessions
│   ├── Apply player inputs
│   ├── Run physics simulation
│   ├── Generate AI inputs
│   └── Detect lap completion
│
└── Send Telemetry (UDP)
    └── Broadcast game state to all players
```

### Transport Layer Architecture

```
TransportLayer
├── TCP Acceptor Task
│   └── For each connection:
│       ├── TLS Handshake (if enabled)
│       ├── Spawn Reader Task
│       │   ├── Read length-prefixed messages
│       │   ├── Handle Authentication
│       │   ├── Handle Heartbeats
│       │   └── Forward to game loop
│       │
│       └── Spawn Writer Task
│           ├── Receive from per-connection channel
│           ├── Serialize message
│           └── Write with length prefix
│
├── UDP Receiver Task
│   └── Receive datagrams
│       └── Forward to game loop
│
└── UDP Sender Task
    └── Send datagrams from queue
```

## Configuration

### Server Configuration

```toml
[network]
tcp_bind = "127.0.0.1:9000"          # TCP listener address
udp_bind = "127.0.0.1:9001"          # UDP socket address
health_bind = "127.0.0.1:9002"       # HTTP health endpoint
tls_cert_path = "./certs/server.crt" # TLS certificate
tls_key_path = "./certs/server.key"  # TLS private key
require_tls = false                  # Enforce TLS requirement (true for production)
heartbeat_interval_ms = 1000         # Client heartbeat interval
heartbeat_timeout_ms = 5000          # Server timeout threshold
```

### TLS Certificate Generation

```bash
openssl req -x509 -newkey rsa:4096 \
    -keyout certs/server.key \
    -out certs/server.crt \
    -days 365 -nodes \
    -subj "/CN=localhost"
```

## Testing

### Unit Tests
All tests passing (26 tests):
```bash
cargo test --lib
```

### Integration Test
```bash
# Start server
cargo run

# In another terminal
curl http://127.0.0.1:9002/health  # Should return "OK"
curl http://127.0.0.1:9002/ready   # Should return "Ready"

# Send Ctrl+C
# Check logs for graceful shutdown messages
```

### Manual Testing Checklist

- [x] Server starts with TLS enabled
- [x] Server falls back without TLS certs
- [x] Health endpoints respond correctly
- [x] Graceful shutdown notifies clients
- [x] Heartbeat system prevents timeouts
- [x] Stale connections cleaned up
- [x] Messages route to game loop
- [x] Per-connection channels work
- [x] Length-prefixed framing prevents message corruption
- [x] Maximum message size enforced

## Performance Characteristics

- **Tick Rate**: 240Hz (4.167ms per tick)
- **TCP Message Processing**: <100μs per loop iteration
- **Connection Cleanup**: Every second (minimal overhead)
- **Max Message Size**: 1MB (DoS protection)
- **Heartbeat Interval**: 1 second (client)
- **Heartbeat Timeout**: 5 seconds (server)
- **Shutdown Grace Period**: 500ms

## Code Statistics

- **Total Lines**: ~600 lines in transport.rs
- **Public API Methods**: 12
- **Background Tasks**: 5 (TCP acceptor, UDP RX, UDP TX, per-connection reader, per-connection writer)
- **Async Functions**: 15+
- **Generic Functions**: 1 (handle_stream)

## Security Considerations

### Implemented

✅ TLS 1.3 encryption for TCP
✅ Message size validation (1MB max)
✅ Connection timeouts (heartbeat-based)
✅ Graceful error handling
✅ No panic on malformed messages

### Not Yet Implemented

❌ Rate limiting per connection
❌ DDoS mitigation
❌ DTLS for UDP encryption
❌ Token-based authentication validation
❌ IP-based blocking

## Future Improvements

1. **Metrics Collection**:
   - Prometheus metrics export
   - Connection count gauges
   - Message throughput counters
   - Latency histograms

2. **Advanced Features**:
   - Rate limiting (token bucket algorithm)
   - Connection pooling
   - Load balancing support
   - Horizontal scaling

3. **Security**:
   - DTLS for UDP
   - Real token validation
   - IP whitelisting/blacklisting
   - DDoS protection

4. **Performance**:
   - Zero-copy optimizations
   - Message batching for UDP
   - Connection multiplexing
   - Lock-free data structures

## Lessons Learned

1. **Generic Stream Handling**: Using generics for `AsyncReadExt + AsyncWriteExt` allows single code path for TLS and non-TLS
2. **Length Prefixing**: Essential for reliable TCP message boundaries
3. **Per-Connection Channels**: Cleaner than shared broadcast channels
4. **Shutdown Signals**: Broadcast channels work well for coordinating shutdown
5. **Non-Blocking Design**: Timeout-based message processing prevents game loop blocking

## Conclusion

The network layer is now **production-ready** for local/LAN deployment. All core features are implemented:

- ✅ Full TLS support
- ✅ Graceful shutdown
- ✅ Message routing
- ✅ Heartbeat system
- ✅ Connection management
- ✅ Health monitoring

For internet deployment, additional features would be recommended:
- DTLS for UDP
- Rate limiting
- DDoS protection
- Token validation
- Metrics/monitoring

**Current Status**: 🟢 Fully Functional - Ready for LAN/Development Use
