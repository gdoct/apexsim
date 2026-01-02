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
2. **Unreal Engine Client (`Project/` + `game/`):** Provides the player experience—menus, HUD, driving view, and integrations with the backend. The [game/README.md](game/README.md) and [Project/README.md](Project/README.md) files describe Unreal-specific workflows.

This separation keeps critical simulation logic isolated from presentation while enabling each component to evolve independently.

## Repository Layout

```
apexsim/
├── content/        # Reference car/track definitions shared across tools
├── Docs/           # System, scene, and architecture documentation
├── game/           # Developer docs, helper scripts, Unreal workflows
├── Project/        # ApexSim Unreal Engine project (source, configs, assets)
├── Scripts/        # Workspace-level helper scripts (build, VS Code generation)
├── server/         # Rust backend (source, config, docs)
├── README.md       # This overview
└── LICENSE         # Project license
```

### Directory Highlights

- [content/](content): Authoring-ready data for cars and tracks consumed by both the server and Unreal editors.
- [Docs/](Docs): High-level design narratives, including scene walkthroughs and subsystem guides.
- [game/](game): Platform-specific setup instructions, Unreal automation helpers, and workflow notes for building and launching the client.
- [Project/](Project): The actual Unreal Engine project containing Blueprints, Maps, Config, and build artifacts.
- [Scripts/](Scripts): Convenience scripts for regenerating IDE files or building aggregate artifacts.
- [server/](server): Full Rust crate with source code, configuration files, and supporting docs for the backend runtime.

## Getting Started

1. Clone the repository and consult [server/README.md](server/README.md) for backend prerequisites, configuration, and run instructions.
2. Follow [game/SETUP.md](game/SETUP.md) and [game/BUILD.md](game/BUILD.md) to provision Unreal Engine, download dependencies, and launch the client.
3. Review [Docs/ARCHITECTURE.md](Docs/ARCHITECTURE.md) plus scene-specific documents under [Docs/Scenes/](Docs/Scenes) for UX flows, and [Docs/Systems/](Docs/Systems) for subsystem expectations.

## Contributing

Contributions are welcome across gameplay programming, engine tooling, networking, UI, and content creation. Please coordinate significant changes via issues or discussion threads, and keep server and client documentation up to date when workflows change. Refer to [server/README.md](server/README.md) and the Unreal documentation in [game/](game) before submitting pull requests.

## License

This project is licensed under the [MIT License](LICENSE).

---