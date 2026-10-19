# [ApexSim](https://gdoct.github.io/apexsim/)

ApexSim is a source-available sim-racing platform: a high-frequency
authoritative server written in Rust and an Unreal Engine 5 client, with a
content pipeline that builds real-world circuits from public data.

[![ApexSim](apexsim/game-unreal/apexsim_splash.png)](https://gdoct.github.io/apexsim/)

The site at **https://gdoct.github.io/apexsim/** shows the cars, the
circuits and the game in motion.

## What it is

- **One simulation, on the server.** The server runs every car at 420 Hz: a
  four-wheel model with per-wheel loads, a slip-based tyre model with heat,
  wear and compounds, aero that depends on ride height and the air, fuel,
  brake and engine heat, damage, hybrids and slipstream. The client has no
  physics: every car, the player's included, is drawn from the server's
  telemetry. The simulation is deterministic, and a test holds it to
  bit-identical runs.
- **Racing.** Races over laps or time, qualifying that sets the grid, hotlaps
  with a ghost, pit stops, weather and time of day that change during a
  session, a track that rubbers in, AI drivers that pass, defend and plan
  stops, and force feedback worked out from the tyre forces.
- **Real circuits.** Every circuit is a measured centerline with real
  elevation, dressed from OpenStreetMap: its stands, buildings, pit lane,
  woods and barriers. The track pipeline bakes each circuit for the client,
  and the game builds it at runtime.
- **Moddable.** Cars are a `car.toml` plus models, the HUD is JSON components,
  and tracks and cars from a player's own Assetto Corsa install can be
  imported. See [MODDING.md](apexsim/MODDING.md).

The project is in active development. What is not built yet is listed in
[apexsim/docs/simulation-gaps.md](apexsim/docs/simulation-gaps.md),
[apexsim/docs/follow-ups.md](apexsim/docs/follow-ups.md) and [apexsim/docs/roadmap.md](apexsim/docs/roadmap.md).

## Playing

Download the latest zip from the
[releases page](https://github.com/gdoct/apexsim/releases), unzip it and run
`launcher.exe`. It starts the game, with a local server if you want one, and
edits the network and graphics settings. To host for others, run
`Server\apexsim-server.exe` ([apexsim/docs/server/operations.md](apexsim/docs/server/operations.md)).

### System requirements

- Windows 10 or 11, 64-bit (x64). Windows on Arm (Snapdragon X) is not
  supported.
- A DirectX 12 GPU. A discrete graphics card is recommended. Integrated
  graphics may run the game on the Low preset, which the game picks on its own
  for an integrated GPU, but this has not been tested.
- 16 GB of RAM.
- 4 GB of free disk space (the game is about 1.6 GB unpacked).
- A wheel, a gamepad or a keyboard.

There is no CPU (software) rendering: the game draws through the GPU.

## Building from source

You need Rust, Python 3 (numpy, Pillow, PyYAML) and Unreal Engine 5.8 on
Windows. In short:

```powershell
cd apexsim/server; cargo run               # in its own terminal: the server (TCP 9000, UDP 9001, HTTP 9002)
cd apexsim                                 # the scripts run from the code root
./scripts/initialize_content.ps1           # build the generated content a fresh clone lacks
./scripts/play_editor.ps1 -Build           # build the client and play it from the editor build
./scripts/build_release.ps1 -Zip           # a release package
```

[apexsim/docs/building.md](apexsim/docs/building.md) has the details: the content stages,
tests, packaging and the engine lookup.

## Performance

Measured with `cargo bench --bench physics_tick` (release, one core, Monza):
one car's physics step takes about 2.4 µs, and a whole 8-car session tick
(physics, AI, collisions, timing) about 40 µs, under 2% of the 2.4 ms a
420 Hz tick allows.

## Repository

The repository holds two roots, each to become its own repository. The code
depends on the content (as the sibling `../apexsim-content`), never the other
way round.

```
apexsim/                code
  server/               Rust server: simulation, sessions, networking, admin dashboard
  game-unreal/          Unreal Engine 5.8 client (C++ modules ApexSim, ApexSimNet, ApexSimInput, ApexSimBoot, ApexTrackEditor)
  hud/                  the HUD components and layouts
  scripts/              build, release, showcase, site and verification scripts
  launcher/             the Windows launcher shipped with releases
  site/                 the marketing site's source (generated into docs/)
  docs/                 documentation of the code
apexsim-content/        content and its tools
  cars/ tracks/ props/  cars, circuits and props, shared by server, pipeline and client
  textures/ wheels/     ground textures and steering wheels
  track-editor/         Rust track pipeline (track-core, the ats-* tools) and the Bevy scene editor
  tools/                the Python content pipeline, importers and generators
  build/tracks/         the track exports the client loads (generated)
  docs/                 documentation of the content and its pipeline
```

## Documentation

[apexsim/docs/README.md](apexsim/docs/README.md) indexes it: architecture, building, the
server, the client and the content pipeline (in
[apexsim-content/docs/](apexsim-content/docs/)).

## Contributing

Coordinate significant changes in an issue first. Before a pull request:

```bash
cd apexsim/server
cargo fmt && cargo clippy --all-targets   # CI enforces fmt --check and -D warnings
cargo test
```

Wire-format changes are made on both sides at once (`apexsim/server/src/network.rs`
and `apexsim/game-unreal/Source/ApexSimNet`) and pinned by golden bytes on both sides;
see [apexsim/docs/server/protocol.md](apexsim/docs/server/protocol.md). Keep the doc for the
area you change up to date.

## License

ApexSim is proprietary software released under the
[ApexSim Restricted Commercial License](LICENSE). You may run, read and modify
it for your own personal, non-commercial use; any commercial use,
redistribution or hosting as a service requires a separate commercial license.
See LICENSE for the terms and contact details.
