# Interactive Test Runner - Quick Start

## Launch the Test Runner

```bash
cd /home/guido/apexsim/server
./run-tests.sh
```

## Interface Overview

The test runner now has a **two-level menu system** organized by topic:

1. **Category Menu**: Browse test categories by topic
2. **Test Menu**: Select individual tests within a category

## What's Included

✅ **All 29 integration tests** organized into 5 categories:

### 1. Integration Tests (7 tests)
Core server integration tests
- Server Initialization
- CLI Client Workflow
- Multiplayer Race Session
- Telemetry Broadcast
- Tick Rate Stress
- Multi-Client Load
- Sandbox Session Workflow

### 2. Lobby & Session Tests (13 tests)
Lobby management and session handling
- Create Session
- Join Session
- Leave Session
- Session Cleanup on Empty
- Max Players Limit
- Rapid Join/Leave
- Multiple Sessions
- Join Nonexistent Session
- Lobby State Broadcast
- Player Returns to Lobby
- Session Kinds
- Disconnect Cleanup
- Demo Mode Lap Timing

### 3. Demo Lap Tests (1 test)
Demo mode and lap timing validation
- Demo Lap Timing

### 4. TLS & Security Tests (4 tests)
TLS configuration and security validation
- TLS Not Required (Starts OK)
- TLS Required (Fails Without Certs)
- TLS Required (Starts With Certs)
- TLS State Logging

### 5. Transport & Performance Tests (4 tests)
Network transport and backpressure handling
- Bounded Channels Prevent OOM
- Droppable Messages Dropped
- Message Priority Classification
- Metrics Tracking

## Usage

### Start Server (for [S] tests)
Many tests need a running server. Start it first:
```bash
cargo run --release
```

### Navigation

**Category Menu:**
- **↑/↓**: Navigate through categories
- **Enter**: Select category
- **Q**: Quit

**Test Menu:**
- **↑/↓**: Navigate through tests
- **Enter**: Run selected test
- **Backspace**: Back to category menu
- **Q**: Quit

**Test Running:**
- **C**: Cancel running test
- **Q**: Quit

**Results View:**
- **↑/↓**: Scroll one line
- **PageUp/PageDown**: Scroll by page
- **Backspace**: Back to test menu
- **Q**: Quit

### Test Indicators
- **[S]**: Requires running server on 127.0.0.1:9000
- **No marker**: Spawns its own server

## Features
- 📂 **Organized by category**: Tests grouped by topic (Integration, Lobby, Demo, TLS, Transport)
- 🎯 **Two-level navigation**: Category → Test selection
- 📺 **Real-time output**: Watch test output as it streams
- ⏸️ **Cancel anytime**: Press 'C' during execution
- 📜 **Scrollable output**: Navigate results with arrow keys and PageUp/PageDown
- 🎨 **Color-coded UI**: Clear visual feedback
- 🔍 **Inline descriptions**: See what each test does
- ⬅️ **Easy navigation**: Backspace to go back, Enter to select

Enjoy testing! 🏎️💨
