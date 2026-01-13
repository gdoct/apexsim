# ApexSim Server Integration Tests

This directory contains integration tests for the ApexSim racing server. These tests verify end-to-end functionality by simulating real client connections.

## Running the Tests

### Interactive Test Runner (Recommended)

The easiest way to run integration tests is using the interactive test runner:

```bash
cd /home/guido/apexsim/server
./run-tests.sh
```

Or directly:
```bash
cargo run --bin test-runner
```

**Features:**
- **Organized by category**: 29 tests grouped into 5 topic-based categories
- **Two-level navigation**: Category menu → Test menu
- **Interactive UI**: Clean ncurses-like terminal interface
- **Navigate with arrow keys**: ↑/↓ to navigate, Enter to select, Backspace to go back
- **Real-time output**: Watch test output as it executes
- **Cancel execution**: Press 'C' during test execution to cancel
- **Scroll output**: Use ↑/↓ or PageUp/PageDown to scroll through test results
- **Server indicators**: Tests marked with `[S]` require a running server

**Server Requirements:**
- Tests marked with `[S]` require the server running on `127.0.0.1:9000`
- Other tests spawn their own server instances automatically

### Prerequisites

1. **For tests requiring a server** (marked with `[S]` in interactive runner):
   ```bash
   cd /home/guido/apexsim/server
   cargo run --release
   ```

2. Wait for the server to display: `Server is running. Press Ctrl+C to stop.`

### Manual Test Execution (Alternative)

You can also run tests manually using cargo commands. This is useful for CI/CD or automated testing:

#### Available Tests

#### 1. CLI Client Workflow Test
Tests the complete client workflow that the CLI game client should follow:
- Connect and authenticate
- Select a car
- Create a session
- Start the race
- Race for 5 seconds
- Return to lobby

```bash
cd /home/guido/apexsim/server
cargo test --test integration_test test_cli_client_workflow -- --ignored --nocapture
```

**This test specifically validates the workflow described in the issue where the client times out during lobby creation.**

#### 2. Multiplayer Race Session Test
Tests 4 clients racing together:

```bash
cargo test --test integration_test test_multiplayer_race_session -- --ignored --nocapture
```

#### 3. Telemetry Broadcast Test
Verifies that all clients receive telemetry broadcasts:

```bash
cargo test --test integration_test test_telemetry_broadcast -- --ignored --nocapture
```

#### 4. Tick Rate Stress Test
Tests server performance at various tick rates (120Hz, 240Hz, 480Hz, 960Hz, 1440Hz):

```bash
cargo test --test integration_test test_tick_rate_stress -- --ignored --nocapture
```

**Note:** This test spawns its own server instances and does not require a pre-running server.

#### 5. Multi-Client Load Test
Tests 16 concurrent clients at various tick rates:

```bash
cargo test --test integration_test test_multi_client_load -- --ignored --nocapture
```

**Note:** This test spawns its own server instances and does not require a pre-running server.

## Test Architecture

### TestClient
The `TestClient` struct simulates a game client with these capabilities:
- TCP connection management
- UDP socket for high-frequency data (optional)
- Authentication with the server
- Car selection
- Session creation and joining
- Player input simulation
- Telemetry reception

### TestClientMinimal
A lightweight version of `TestClient` used for stress tests with minimal overhead.

## Common Issues

### Connection Timeout
If you see connection timeouts:
1. Verify the server is running on `127.0.0.1:9000`
2. Check that no firewall is blocking the connection
3. Ensure the server config uses the correct bind addresses

### Test Timeout
If tests timeout after 30 seconds:
1. Check server logs for errors
2. Verify the server is processing messages
3. Look for deadlocks or infinite loops in server code

### Heartbeat Timeout (6 seconds)
This is the issue described in the original problem. The server expects heartbeat messages every 6 seconds (configured as `heartbeat_timeout_ms: 6000`). If a client doesn't send heartbeats:
- The server will disconnect the client after 6 seconds
- The client will see: `Connection timed out (player: CLI-Player)`

The CLI client should send heartbeats periodically (every 2 seconds is recommended).

## Debugging Tips

### Enable Detailed Logging
Run tests with `RUST_LOG` environment variable:

```bash
RUST_LOG=debug cargo test --test integration_test test_cli_client_workflow -- --ignored --nocapture
```

### View Server Logs
In a separate terminal, tail the server logs:

```bash
tail -f /home/guido/apexsim/server/server.log
```

### Manual Testing
You can also run the CLI client manually to debug issues:

```bash
cd /home/guido/apexsim/cli-game
cargo run --release
```

## Test Coverage

The integration tests cover:
- ✅ Authentication flow
- ✅ Lobby state management
- ✅ Car selection
- ✅ Session creation and joining
- ✅ Race start sequence
- ✅ Telemetry broadcast to all participants
- ✅ Player input handling
- ✅ Session completion and lobby return
- ✅ Heartbeat mechanism
- ✅ Multi-client synchronization
- ✅ High tick rate performance (up to 1440Hz)
- ✅ Concurrent client load (16+ clients)

## CI/CD Integration

To run these tests in CI/CD:

```yaml
# Example GitHub Actions workflow
- name: Start ApexSim Server
  run: |
    cd server
    cargo run --release &
    sleep 5  # Wait for server to start

- name: Run Integration Tests
  run: |
    cd server
    cargo test --test integration_test -- --ignored --nocapture

- name: Stop Server
  run: pkill apexsim-server
```
