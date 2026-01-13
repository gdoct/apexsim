# Test Categories Overview

The interactive test runner organizes all 29 integration tests into 5 logical categories.

## Navigation Flow

```
Category Menu
    ↓ (Select category)
Test Menu
    ↓ (Select test)
Running Test
    ↓ (Complete or cancel)
Results View
    ↓ (Backspace)
Test Menu (or Category Menu)
```

## Category Breakdown

### 📦 Category 1: Integration Tests
**7 tests** - Core server integration tests

| Test | Server Required | Description |
|------|----------------|-------------|
| Server Initialization | No | Basic server startup test |
| CLI Client Workflow | Yes [S] | Complete client workflow: auth, car select, race |
| Multiplayer Race Session | Yes [S] | 4 clients racing together |
| Telemetry Broadcast | Yes [S] | Verify clients receive telemetry broadcasts |
| Tick Rate Stress | No | Test various tick rates (120Hz-1440Hz) |
| Multi-Client Load | No | 16 concurrent clients load test |
| Sandbox Session Workflow | Yes [S] | Sandbox mode session workflow |

---

### 🎮 Category 2: Lobby & Session Tests
**13 tests** - Lobby management and session handling

| Test | Server Required | Description |
|------|----------------|-------------|
| Create Session | Yes [S] | Session creation flow |
| Join Session | Yes [S] | Join existing session |
| Leave Session | Yes [S] | Leave session flow |
| Session Cleanup on Empty | Yes [S] | Automatic session cleanup |
| Max Players Limit | Yes [S] | Session player limit enforcement |
| Rapid Join/Leave | Yes [S] | Rapid join/leave operations |
| Multiple Sessions | Yes [S] | Multiple concurrent sessions |
| Join Nonexistent Session | Yes [S] | Error handling for invalid session |
| Lobby State Broadcast | Yes [S] | Lobby state updates to all clients |
| Player Returns to Lobby | Yes [S] | Return to lobby after race |
| Session Kinds | Yes [S] | Different session types |
| Disconnect Cleanup | Yes [S] | Cleanup on client disconnect |
| Demo Mode Lap Timing | Yes [S] | Lap timing in demo mode |

---

### 🏁 Category 3: Demo Lap Tests
**1 test** - Demo mode and lap timing validation

| Test | Server Required | Description |
|------|----------------|-------------|
| Demo Lap Timing | No | Demo lap timing accuracy |

---

### 🔒 Category 4: TLS & Security Tests
**4 tests** - TLS configuration and security validation

| Test | Server Required | Description |
|------|----------------|-------------|
| TLS Not Required (Starts OK) | No | Server starts without TLS when not required |
| TLS Required (Fails Without Certs) | No | Server fails without TLS when required |
| TLS Required (Starts With Certs) | No | Server starts with TLS when certs exist |
| TLS State Logging | No | TLS state logging |

---

### 🚀 Category 5: Transport & Performance Tests
**4 tests** - Network transport and backpressure handling

| Test | Server Required | Description |
|------|----------------|-------------|
| Bounded Channels Prevent OOM | No | Backpressure prevents memory issues |
| Droppable Messages Dropped | No | Message dropping when queue full |
| Message Priority Classification | No | Message priority handling |
| Metrics Tracking | No | Metrics collection |

---

## Statistics

- **Total Tests**: 29
- **Tests Requiring Server [S]**: 17 (59%)
- **Standalone Tests**: 12 (41%)

### By Category:
1. Integration Tests: 7 tests (4 require server, 3 standalone)
2. Lobby & Session Tests: 13 tests (all require server)
3. Demo Lap Tests: 1 test (standalone)
4. TLS & Security Tests: 4 tests (all standalone)
5. Transport & Performance Tests: 4 tests (all standalone)

## Benefits of Category Organization

✅ **Better Organization**: Related tests grouped together
✅ **Easier Navigation**: Find tests by topic, not by scrolling through 29 items
✅ **Clear Context**: Category descriptions explain what each group tests
✅ **Scalable**: Easy to add more tests to existing categories or create new ones
✅ **Standard Terminal Size**: Works perfectly on standard 80x24 terminals
