# ApexSim SimRacing Platform

ApexSim is an open-source simracing platform composed of a high-frequency authoritative server written in Rust and a rich Unreal Engine client. The codebase is tuned for realistic vehicle physics, low-latency multiplayer, and mod-friendly content pipelines.

## PROJECT STATUS
This project is in active development. Core simulation features are functional, but many gameplay systems, UI elements, and polish remain works in progress.
### Working parts so far:
* 25 tracks with exact measured center-line spline, track width and racing line. elevation data is missing for the tracks.
* 5 car models with physics and 3d model
* authoritative server with sophisticated physics and networking (authoritative means the server decides where each car is)
* server ticks at 240hz by default but should be reliable up to 1Mhz
* supports up to 20 players or AI drivers per session
* godot implementation of the client with network logon, lobby management, and basic track view
* basic ui for lobby, car selection, track selection

### Missing
* car control implementation in the godot client
* car view in the godot client
* sound implementation in the godot client
* lap timing
.. and many more features

## Architecture Overview

1. **Rust Server (`server/`):** Runs the authoritative 240 Hz simulation loop, manages sessions, performs collision-aware physics, and streams telemetry via UDP while handling lobby/stateful traffic over TCP. See [server/README.md](server/README.md) for configuration, build, and operations detail.
2. **Godot Client (`game-godot/` + `game-cli/`):** Provides the player experience—menus, HUD, driving view, and integrations with the backend. The [game-cli/README.md](game/README.md) and [game-godot/README.md](game-godot/README.md) files describe Godot-specific workflows.

This separation keeps critical simulation logic isolated from presentation while enabling each component to evolve independently.

### Serialization

The client and server communicate using a lightweight, cross-platform binary serialization format called [MessagePack](https://msgpack.org/). All networked data structures are defined in Rust with `serde` and `rmp_serde` for efficient, schema-aware encoding and decoding. This choice prioritizes performance and low bandwidth overhead, which is critical for real-time simulation.

## Repository Layout

```
apexsim/
├── content/        # Reference car/track definitions shared across tools
├── game-godot/     # Game implementation in godot with c# scripts
├── game-cli/       # Command line client for integration testing
├── scripts/        # Workspace-level helper scripts (build, VS Code generation)
├── server/         # Rust backend (source, config, docs)
├── README.md       # This overview
└── LICENSE         # Project license
```

### Directory Highlights

- [content/](content): Authoring-ready data for cars and tracks consumed by both the server and game clients.
- [game-godot/](game-godot): Game implementation in godot with c# scripts.
- [game-cli/](game-cli): Command line client for integration testing.
- [scripts/](scripts): Workspace-level helper scripts (build, VS Code generation).
- [server/](server): Full Rust crate with source code, configuration files, and supporting docs for the backend runtime.

## Getting Started

1. Clone the repository and consult [server/README.md](server/README.md) for backend prerequisites, configuration, and run instructions.
2. Follow [game-godot/SETUP.md](game-godot/SETUP.md) and [game-godot/BUILD.md](game-godot/BUILD.md) to provision Godot, download dependencies, and launch the client.

## Contributing

Contributions are welcome across gameplay programming, engine tooling, networking, UI, and content creation. Please coordinate significant changes via issues or discussion threads, and keep server and client documentation up to date when workflows change. Refer to [server/README.md](server/README.md) and the Unreal documentation in [game/](game) before submitting pull requests.

## License

This project is licensed under the [MIT License](LICENSE).

---