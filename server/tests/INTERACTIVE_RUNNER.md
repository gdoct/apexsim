# Interactive Test Runner Guide

## Quick Start

```bash
cd /home/guido/apexsim/server
./run-tests.sh
```

Or run directly:
```bash
cargo run --bin test-runner
```

## Interface Overview

The interactive test runner provides an ncurses-like terminal interface with a **two-level menu system** organized by topic.

### Level 1: Category Menu

First, you select a test category:

```
╔════════════════════════════════════════════════════════════════════════════╗
║            ApexSim Server - Interactive Integration Tests                ║
╚════════════════════════════════════════════════════════════════════════════╝

  ↑/↓: Navigate  │  Enter: Select Category  │  Q: Quit

  1. Integration Tests                            (7 tests)
     Core server integration tests

  2. Lobby & Session Tests                        (13 tests)
     Lobby management and session handling

  3. Demo Lap Tests                               (1 test)
     Demo mode and lap timing validation

  4. TLS & Security Tests                         (4 tests)
     TLS configuration and security validation

  5. Transport & Performance Tests                (4 tests)
     Network transport and backpressure handling

  Total: 29 tests across 5 categories
```

### Level 2: Test Menu

Then, you select a specific test within that category:

```
╔════════════════════════════════════════════════════════════════════════════╗
║ Lobby & Session Tests                                                     ║
╚════════════════════════════════════════════════════════════════════════════╝

  ↑/↓: Navigate  │  Enter: Run Test  │  Backspace: Back  │  Q: Quit

 [S]  1. Create Session
     Session creation flow

 [S]  2. Join Session
     Join existing session

 [S]  3. Leave Session
     Leave session flow

  ... (more tests)

  [S] = Requires running server on 127.0.0.1:9000
```

### Level 3: Running Test Screen

When you press Enter on a test, you'll see:

```
Running: CLI Client Workflow
Category: Lobby & Session Tests │ File: lobby_integration_tests
────────────────────────────────────────────────────────────────────────────────
   Compiling apexsim-server v0.1.0
    Finished test [unoptimized + debuginfo] target(s) in 1.23s
     Running tests/integration_test.rs

running 1 test
test test_cli_client_workflow ... ok

test result: ok. 1 passed; 0 failed; 0 ignored; 0 measured; 0 filtered out

────────────────────────────────────────────────────────────────────────────────
Press 'C' to cancel test  │  Press 'Q' to quit
```

### Level 4: Test Results Screen

After test completion:

```
Test Completed: CLI Client Workflow
────────────────────────────────────────────────────────────────────────────────
[... scrollable output ...]

────────────────────────────────────────────────────────────────────────────────
↑/↓: Scroll │ Lines 1-47/47 │ Backspace: Back to Menu │ Q: Quit
```

## Keyboard Controls

### Category Menu (Level 1)
- **↑/↓ Arrow Keys**: Navigate through categories
- **Enter**: Select category and go to test menu
- **Q**: Quit the application

### Test Menu (Level 2)
- **↑/↓ Arrow Keys**: Navigate through tests in the category
- **Enter**: Run the selected test
- **Backspace**: Go back to category menu
- **Q**: Quit the application

### Running Test
- **C**: Cancel the currently running test
- **Q**: Quit (will also cancel the test)

### Test Results View (Level 4)
- **↑/↓ Arrow Keys**: Scroll through output one line at a time
- **PageUp/PageDown**: Scroll through output by page
- **Backspace**: Go back to test menu
- **Q**: Quit the application

## Test Categories

### Tests Requiring Running Server `[S]`

These tests require you to start the ApexSim server before running:

```bash
cd /home/guido/apexsim/server
cargo run --release
```

Tests marked with `[S]` connect to `127.0.0.1:9000` and expect the server to be available.

Examples:
- CLI Client Workflow
- Multiplayer Race Session
- Telemetry Broadcast
- All lobby integration tests
- Sandbox Session Workflow

### Standalone Tests (No `[S]` marker)

These tests spawn their own server instances and don't require a pre-running server:

Examples:
- Server Initialization
- Tick Rate Stress
- Multi-Client Load
- All TLS requirement tests
- All transport backpressure tests
- Demo lap tests

## Features

### Real-Time Output
- Watch test output as it streams in real-time
- See compilation progress, test execution, and results live

### Output Scrolling
- After test completion, scroll through all output
- Navigate with arrow keys or page up/down
- See line numbers and total lines at the bottom

### Test Cancellation
- Press 'C' during test execution to cancel
- Safely kills the test process
- Returns to results view with cancellation message

### Clean Navigation
- Selected test is highlighted in blue
- Test descriptions shown for selected test
- Server requirement indicators for easy identification
- Smooth navigation between screens

## Complete Test List (29 Total)

### Category 1: Integration Tests (7 tests)
Core server integration tests
1. Server Initialization
2. CLI Client Workflow [S]
3. Multiplayer Race Session [S]
4. Telemetry Broadcast [S]
5. Tick Rate Stress
6. Multi-Client Load
7. Sandbox Session Workflow [S]

### Category 2: Lobby & Session Tests (13 tests)
Lobby management and session handling
1. Create Session [S]
2. Join Session [S]
3. Leave Session [S]
4. Session Cleanup on Empty [S]
5. Max Players Limit [S]
6. Rapid Join/Leave [S]
7. Multiple Sessions [S]
8. Join Nonexistent Session [S]
9. Lobby State Broadcast [S]
10. Player Returns to Lobby [S]
11. Session Kinds [S]
12. Disconnect Cleanup [S]
13. Demo Mode Lap Timing [S]

### Category 3: Demo Lap Tests (1 test)
Demo mode and lap timing validation
1. Demo Lap Timing

### Category 4: TLS & Security Tests (4 tests)
TLS configuration and security validation
1. TLS Not Required (Starts OK)
2. TLS Required (Fails Without Certs)
3. TLS Required (Starts With Certs)
4. TLS State Logging

### Category 5: Transport & Performance Tests (4 tests)
Network transport and backpressure handling
1. Bounded Channels Prevent OOM
2. Droppable Messages Dropped
3. Message Priority Classification
4. Metrics Tracking

## Tips

1. **Start the server first**: If you plan to run multiple `[S]` tests, start the server once and keep it running
2. **Use scrolling**: Test output can be long - use PageUp/PageDown for faster navigation
3. **Cancel long tests**: If a test is taking too long, press 'C' to cancel and return to the menu
4. **Check descriptions**: Each test shows a description when selected to help you understand what it tests

## Troubleshooting

### "Connection refused" errors
- Make sure the server is running for tests marked with `[S]`
- Check that the server is bound to `127.0.0.1:9000`

### Test hangs or times out
- Press 'C' to cancel the test
- Check server logs for issues
- Verify no other process is using ports 9000/9001

### Terminal display issues
- Try resizing your terminal window
- Ensure your terminal supports ANSI colors
- Use a modern terminal emulator (not Windows Command Prompt)

## Exit Codes

- **0**: Normal exit
- **Non-zero**: Error occurred (check terminal output)
