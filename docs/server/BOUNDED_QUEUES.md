# Bounded Network Queues and Drop/Backpressure Policy

## Overview

The ApexSim server implements bounded network queues to prevent Out-of-Memory (OOM) errors under slow clients or high load conditions. This document describes the implementation and policies.

## Channel Capacity

All major MPSC (Multi-Producer Single-Consumer) channels in the transport layer use bounded channels with the following capacities:

| Channel Type | Capacity | Purpose |
|--------------|----------|---------|
| TCP Inbound | 1,000 messages | Client -> Server messages via TCP |
| UDP Inbound | 2,000 messages | Client -> Server messages via UDP |
| UDP Outbound | 2,000 messages | Server -> Client messages via UDP |
| Per-Client TCP Outbound | 100 messages | Server -> Specific Client via TCP |
| Shutdown | Unbounded | Critical shutdown signals (low volume) |

## Message Priority

Messages are classified into two priority levels:

### Critical Messages (MessagePriority::Critical)

These messages **must** be delivered or the client should be disconnected:

- `AuthSuccess` - Authentication succeeded
- `AuthFailure` - Authentication failed
- `Error` - Error messages
- `SessionJoined` - Session join confirmation
- `SessionStarting` - Session countdown started
- `SessionLeft` - Session left confirmation

**Behavior**: When a critical message cannot be sent because the client's queue is full, the server marks the client for disconnection and increments the `clients_disconnected_backpressure` metric.

### Droppable Messages (MessagePriority::Droppable)

These messages **may be dropped** when queues are full:

- `Telemetry` - High-frequency game state updates (240Hz)
- `HeartbeatAck` - Heartbeat acknowledgments
- `LobbyState` - Lobby state broadcasts
- `PlayerDisconnected` - Player disconnection notifications

**Behavior**: When a droppable message cannot be sent because the queue is full, it is silently dropped and the `tcp_messages_dropped` or `udp_messages_dropped` metric is incremented.

## Drop/Backpressure Policy

### For TCP (Per-Client Channels)

1. **Critical Messages**:
   - Use `send()` which awaits until space is available
   - If send fails (channel closed or full after waiting), return `TransportError::QueueFull`
   - Increment `clients_disconnected_backpressure` metric
   - Client should be disconnected by the caller

2. **Droppable Messages**:
   - Use `try_send()` which returns immediately
   - If queue is full, drop the message
   - Increment `tcp_messages_dropped` metric
   - Log every 100th dropped message to avoid log spam
   - Return `Ok(())` - dropping is expected behavior

### For UDP (Global Outbound Channel)

1. **Critical Messages**:
   - Should generally be sent via TCP instead
   - If sent via UDP, use `send()` to await space
   - UDP is unreliable by nature, so critical messages are discouraged

2. **Droppable Messages**:
   - Use `try_send()` which returns immediately
   - If queue is full, drop the message
   - Increment `udp_messages_dropped` metric
   - Log every 1000th dropped message
   - Return `Ok(())` - dropping is expected behavior

## Metrics

The `TransportMetrics` struct tracks:

- `tcp_messages_dropped: AtomicU64` - Total TCP messages dropped due to full queues
- `udp_messages_dropped: AtomicU64` - Total UDP messages dropped due to full queues
- `clients_disconnected_backpressure: AtomicU64` - Clients disconnected due to slow consumption

These metrics are thread-safe and can be queried at runtime to monitor system health.

## Testing

Tests are located in `tests/transport_backpressure_test.rs`:

1. **test_bounded_channels_prevent_oom** - Verifies channels are bounded
2. **test_droppable_messages_are_dropped_when_queue_full** - Verifies drop behavior
3. **test_message_priority_classification** - Verifies message priority assignment
4. **test_metrics_tracking** - Verifies metrics are tracked correctly

## Performance Characteristics

### Under Normal Load

- All channels should remain well below capacity
- No messages should be dropped
- No clients should be disconnected due to backpressure

### Under High Load (Many Clients)

- Droppable messages (especially telemetry at 240Hz) may be dropped for slow clients
- Slow clients will see degraded experience (missed updates) but won't crash the server
- Memory usage remains bounded

### Slow Client Scenario

- If a client cannot keep up with the message rate:
  - Droppable messages (telemetry, heartbeats) are dropped
  - Client may see stuttering or missed updates
  - If critical messages cannot be sent, client is disconnected
  - Server memory remains stable

## Future Improvements

Potential enhancements to consider:

1. **Adaptive Queue Sizes** - Dynamically adjust queue sizes based on load
2. **Priority Queues** - Separate queues for critical vs droppable messages
3. **Rate Limiting** - Limit message sending rate per client
4. **Monitoring Dashboard** - Real-time visualization of metrics
5. **Client-Side Acknowledgment** - Have clients acknowledge receipt of critical messages
