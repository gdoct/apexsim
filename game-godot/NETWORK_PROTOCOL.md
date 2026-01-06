# Network Protocol Implementation

## Current Status

The Godot C# client uses **MessagePack** for serializing all client-server messages. This aligns with the Rust server's protocol, ensuring full compatibility and high performance.

The C# implementation can be found in `scripts/csharp/NetworkClient.cs`.

## Message Format

All messages are prefixed with a 4-byte, big-endian integer representing the length of the payload.

```
[4 bytes: message length (big-endian)] [N bytes: MessagePack data]
```

### Serialization

-   **Client-to-Server:** Outgoing `ClientMessage` objects are serialized into a `Dictionary<string, object>` and then encoded using `MessagePackSerializer`.
-   **Server-to-Client:** Incoming byte arrays are parsed with `MessagePackSerializer` into a `Dictionary<string, object>` and then mapped to the corresponding `ServerMessage` types.

This contract-less approach provides flexibility but relies on matching field names and types between the client and server.

## Future Work

- [ ] Add integration tests between the Rust server and C# client to ensure protocol compatibility.
- [ ] Investigate using contract-based serialization with `[MessagePackObject]` attributes for improved performance and type safety.
- [ ] Evaluate network compression for large telemetry packets.