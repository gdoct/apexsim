# Network Protocol Implementation

## Current Status

The Godot C# client currently uses **JSON** serialization for network messages to simplify implementation.

## Server Compatibility

The ApexSim server uses **bincode** (Rust binary format). For the Godot client to work, you have two options:

### Option 1: Add JSON Support to Server (Recommended for Development)

Add JSON deserialization to the server alongside bincode:

```rust
// In server, detect format and deserialize accordingly
if first_byte == b'{' {
    // JSON format
    serde_json::from_slice(&data)?
} else {
    // bincode format
    bincode::deserialize(&data)?
}
```

### Option 2: Implement Bincode in C# (Production)

Implement a bincode-compatible serializer in C# that matches Rust's format exactly. This requires:

1. Manual serialization following bincode spec
2. Proper enum variant encoding
3. Big-endian integer encoding
4. Matching Rust's struct layout

## Message Format

Both formats use length-prefixed messages:

```
[4 bytes: message length (big-endian)] [N bytes: message data]
```

## Future Work

- [ ] Implement proper bincode serialization in C#
- [ ] Add integration tests between Rust server and C# client
- [ ] Performance benchmarking JSON vs bincode
