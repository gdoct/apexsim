# Timeout Issue Analysis - CLI Client Lobby Creation

## Problem Summary

The CLI game client times out when trying to create a lobby. The client waits a long time and then refreshes the screen with no feedback. The server logs show:

```
2026-01-01T14:16:28.399082Z  INFO apexsim_server: Transport layer initialized successfully
2026-01-01T14:16:28.399105Z  INFO apexsim_server: Server marked as ready
2026-01-01T14:16:28.399120Z  INFO apexsim_server: Server is running. Press Ctrl+C to stop.
2026-01-01T14:16:38.525957Z  INFO apexsim_server::transport: New TCP connection from 127.0.0.1:34418
2026-01-01T14:16:38.526232Z  INFO apexsim_server::transport: Player CLI-Player authenticated as b306d19d-5ef1-42b5-a427-0c517a0cc670 (connection: 8648102366895229938)
2026-01-01T14:16:38.528503Z  INFO apexsim_server::lobby: Player CLI-Player added to lobby
2026-01-01T14:16:44.394589Z  WARN apexsim_server::transport: Connection 8648102366895229938 timed out (player: CLI-Player)
```

**Timeline:**
- T+0s: Connection established
- T+0s: Authentication successful
- T+0.002s: Player added to lobby
- T+6s: Connection timed out

## Root Cause Analysis

### 1. Heartbeat Mechanism

The server expects clients to send heartbeat messages periodically to keep the connection alive.

**Server Configuration** ([server/src/main.rs](server/src/main.rs)):
- Default heartbeat timeout: **6000ms** (6 seconds)
- The server tracks `last_heartbeat` timestamp for each connection

**Server Heartbeat Checking** ([server/src/transport.rs:506-526](server/src/transport.rs#L506-L526)):
```rust
pub async fn cleanup_stale_connections(&self) {
    let now = Instant::now();
    let timeout = self.heartbeat_timeout; // 6000ms by default

    let mut connections = self.connections.write().await;
    let mut to_remove = Vec::new();

    for (conn_id, info) in connections.iter() {
        if now.duration_since(info.last_heartbeat) > timeout {
            warn!("Connection {} timed out (player: {})", conn_id, info.player_name);
            to_remove.push(*conn_id);
        }
    }
    // ... cleanup code
}
```

This function is called every second in the game loop ([server/src/main.rs:542-544](server/src/main.rs#L542-L544)):
```rust
// Cleanup stale connections every second
if tick_count % tick_rate as u64 == 0 {
    transport_write.cleanup_stale_connections().await;
}
```

### 2. Client Heartbeat Implementation

The CLI client has a heartbeat mechanism in `wait_for_response()` ([cli-game/src/main.rs:545-597](cli-game/src/main.rs#L545-L597)):

```rust
async fn wait_for_response(&mut self, timeout: Duration) -> Result<ServerMessage> {
    let start = tokio::time::Instant::now();
    let mut last_heartbeat = tokio::time::Instant::now();

    loop {
        if start.elapsed() > timeout {
            return Err(anyhow::anyhow!("Timeout waiting for response"));
        }

        // Send heartbeat every 2 seconds to keep connection alive
        if last_heartbeat.elapsed() > Duration::from_secs(2) {
            let _ = self.send_heartbeat().await;
            last_heartbeat = tokio::time::Instant::now();
        }
        // ... message receiving code
    }
}
```

**The client sends heartbeats every 2 seconds, which should be sufficient for the 6-second server timeout.**

### 3. The Actual Problem

Looking at the client workflow in [cli-game/src/main.rs:274-351](cli-game/src/main.rs#L274-L351), the `create_session()` function:

1. Collects user input (track, max_players, ai_count, lap_limit)
2. Sends `CreateSession` message
3. Calls `wait_for_response()` which sends heartbeats

**However**, there's a potential issue if the user takes more than 6 seconds to input the session parameters! During the interactive prompts, NO heartbeats are being sent.

The problematic sequence:
```rust
// Get session parameters
let max_players: u8 = Input::new()
    .with_prompt("Max players")
    .default(8)
    .interact()?;  // <-- User input, no heartbeats sent!

let ai_count: u8 = Input::new()
    .with_prompt("AI drivers")
    .default(0)
    .interact()?;  // <-- User input, no heartbeats sent!

let lap_limit: u8 = Input::new()
    .with_prompt("Number of laps")
    .default(5)
    .interact()?;  // <-- User input, no heartbeats sent!
```

If the user takes more than 6 seconds to fill out these prompts, the server will timeout the connection before the `CreateSession` message is even sent!

### 4. Secondary Issue: Missing Error Feedback

When session creation fails, the client doesn't provide clear feedback. In [cli-game/src/main.rs:400-407](cli-game/src/main.rs#L400-L407):

```rust
} else {
    // No car selected
    let _ = transport_write.send_tcp(connection_id, ServerMessage::Error {
        code: 400,
        message: "Must select a car before creating session".to_string(),
    }).await;
}
```

If the client didn't select a car, the server sends an error, but the client's `wait_for_response()` function would still be waiting for a `SessionJoined` message and would timeout.

## Solutions

### Solution 1: Background Heartbeat Task (Recommended)

Implement a background task that continuously sends heartbeats, independent of user interaction:

```rust
// In GameClient::new()
tokio::spawn(async move {
    let mut interval = tokio::time::interval(Duration::from_secs(2));
    loop {
        interval.tick().await;
        let _ = self.send_heartbeat().await;
    }
});
```

### Solution 2: Pre-send Heartbeat Before Long Operations

Before any interactive user input that might take >6 seconds, send a heartbeat:

```rust
async fn create_session(&mut self) -> Result<()> {
    // Send heartbeat before user interaction
    self.send_heartbeat().await?;

    // Now safe to collect user input
    let max_players: u8 = Input::new()...
}
```

### Solution 3: Increase Server Heartbeat Timeout

In `server.toml`, increase the heartbeat timeout to 30 seconds:

```toml
[network]
heartbeat_timeout_ms = 30000  # 30 seconds instead of 6
```

This gives users more time for interactive prompts.

### Solution 4: Better Error Handling

Improve error message handling in the client:

```rust
match response {
    ServerMessage::SessionJoined { ... } => { ... }
    ServerMessage::Error { code, message } => {
        println!("{} Error: {}", style("✗").red(), message);
        return Err(anyhow::anyhow!("Server error {}: {}", code, message));
    }
    other => {
        println!("{} Unexpected response: {:?}", style("!").yellow(), other);
        return Err(anyhow::anyhow!("Unexpected response: {:?}", other));
    }
}
```

## Recommended Fix

Implement **Solution 1** (background heartbeat task) + **Solution 4** (better error handling):

1. **Background heartbeat**: Ensures connection stays alive regardless of user interaction
2. **Better error handling**: Provides clear feedback when things go wrong

This combination makes the client robust against both user delays and server errors.

## Testing

The new integration test `test_cli_client_workflow` in [server/tests/integration_test.rs](server/tests/integration_test.rs) validates the complete workflow:

```bash
# Make sure server is running first
cd /home/guido/apexsim/server
cargo run --release &

# Run the test
cargo test --test integration_test test_cli_client_workflow -- --ignored --nocapture
```

This test:
- ✅ Connects and authenticates
- ✅ Selects a car
- ✅ Creates a session
- ✅ Starts the race
- ✅ Sends heartbeats every 2 seconds during the race
- ✅ Receives telemetry (291 packets in 5 seconds)
- ✅ Returns to lobby
- ✅ Properly handles intermixed telemetry and lobby state messages

**Test Status: ✅ PASSING**

The test validates that the client-server workflow works correctly when heartbeats are sent regularly.

## Additional Notes

### Current Client Behavior

The CLI client sends heartbeats in two ways:

1. **Menu Loop** ([cli-game/src/main.rs:618-620](cli-game/src/main.rs#L618-L620)):
```rust
async fn run_menu(&mut self) -> Result<bool> {
    // Send heartbeat before showing menu
    let _ = self.send_heartbeat().await;
```

2. **Wait for Response** ([cli-game/src/main.rs:556-559](cli-game/src/main.rs#L556-L559)):
```rust
// Send heartbeat every 2 seconds to keep connection alive
if last_heartbeat.elapsed() > Duration::from_secs(2) {
    let _ = self.send_heartbeat().await;
    last_heartbeat = tokio::time::Instant::now();
}
```

**Issue**: During interactive prompts (track selection, max players, etc.), NO heartbeats are sent because the code blocks on user input. If the user takes >6 seconds to answer, the connection times out.

### Heartbeat Interval Recommendation

For a 6-second server timeout:
- ✅ Send heartbeats every 2 seconds (provides 3x safety margin)
- ✅ Implemented in `wait_for_response()` function
- ⚠️ Still vulnerable during blocking user input prompts

### Integration Test Implementation

The integration test now includes heartbeat support:
- Sends heartbeat every 2 seconds during the race loop
- Properly handles `HeartbeatAck` messages from server
- Skips telemetry messages when waiting for lobby state responses
- Validates that 291 telemetry packets are received in 5 seconds (58 Hz effective rate)

This confirms the server's heartbeat mechanism works correctly when clients send regular heartbeats.
