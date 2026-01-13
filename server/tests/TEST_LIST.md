# Complete Test List - Interactive Test Runner

This document confirms all integration tests are available in the interactive test runner.

## Test Coverage Summary

**Total Tests: 29**
- Integration Tests (integration_test.rs): 7 tests
- Lobby Integration Tests (lobby_integration_tests.rs): 13 tests
- Demo Lap Tests (demo_lap_tests.rs): 1 test
- TLS Requirement Tests (tls_requirement_test.rs): 4 tests
- Transport Backpressure Tests (transport_backpressure_test.rs): 4 tests

---

## Integration Tests (integration_test.rs)

| # | Test Name | Function Name | Requires Server |
|---|-----------|---------------|-----------------|
| 1 | Server Initialization | test_server_initialization | No |
| 2 | CLI Client Workflow | test_cli_client_workflow | Yes [S] |
| 3 | Multiplayer Race Session | test_multiplayer_race_session | Yes [S] |
| 4 | Telemetry Broadcast | test_telemetry_broadcast | Yes [S] |
| 5 | Tick Rate Stress | test_tick_rate_stress | No |
| 6 | Multi-Client Load | test_multi_client_load | No |
| 7 | Sandbox Session Workflow | test_sandbox_session_workflow | Yes [S] |

---

## Lobby Integration Tests (lobby_integration_tests.rs) ✓

All 13 lobby integration tests are included:

| # | Test Name | Function Name | Requires Server |
|---|-----------|---------------|-----------------|
| 8 | Create Session | test_create_session | Yes [S] |
| 9 | Join Session | test_join_session | Yes [S] |
| 10 | Leave Session | test_leave_session | Yes [S] |
| 11 | Session Cleanup on Empty | test_session_cleanup_on_empty | Yes [S] |
| 12 | Max Players Limit | test_max_players_limit | Yes [S] |
| 13 | Rapid Join/Leave | test_rapid_join_leave | Yes [S] |
| 14 | Multiple Sessions | test_multiple_sessions | Yes [S] |
| 15 | Join Nonexistent Session | test_join_nonexistent_session | Yes [S] |
| 16 | Lobby State Broadcast | test_lobby_state_broadcast | Yes [S] |
| 17 | Player Returns to Lobby | test_player_returns_to_lobby | Yes [S] |
| 18 | Session Kinds | test_session_kinds | Yes [S] |
| 19 | Disconnect Cleanup | test_disconnect_cleanup | Yes [S] |
| 20 | Demo Mode Lap Timing | test_demo_mode_lap_timing | Yes [S] |

---

## Demo Lap Tests (demo_lap_tests.rs) ✓

| # | Test Name | Function Name | Requires Server |
|---|-----------|---------------|-----------------|
| 21 | Demo Lap Timing | test_demo_lap_timing | No |

---

## TLS Requirement Tests (tls_requirement_test.rs)

| # | Test Name | Function Name | Requires Server |
|---|-----------|---------------|-----------------|
| 22 | TLS Not Required (Starts OK) | test_server_starts_without_tls_when_not_required | No |
| 23 | TLS Required (Fails Without Certs) | test_server_fails_without_tls_when_required | No |
| 24 | TLS Required (Starts With Certs) | test_server_starts_with_tls_when_required_and_certs_exist | No |
| 25 | TLS State Logging | test_tls_state_logging | No |

---

## Transport Backpressure Tests (transport_backpressure_test.rs)

| # | Test Name | Function Name | Requires Server |
|---|-----------|---------------|-----------------|
| 26 | Bounded Channels Prevent OOM | test_bounded_channels_prevent_oom | No |
| 27 | Droppable Messages Dropped | test_droppable_messages_are_dropped_when_queue_full | No |
| 28 | Message Priority Classification | test_message_priority_classification | No |
| 29 | Metrics Tracking | test_metrics_tracking | No |

---

## Verification

All tests from the following files are included:
- ✅ `lobby_integration_tests.rs` - All 13 tests included
- ✅ `demo_lap_tests.rs` - 1 test included
- ✅ `integration_test.rs` - All 7 tests included
- ✅ `tls_requirement_test.rs` - All 4 tests included
- ✅ `transport_backpressure_test.rs` - All 4 tests included

## How to Run

```bash
cd /home/guido/apexsim/server
./run-tests.sh
```

Use arrow keys to navigate and Enter to run any test!
