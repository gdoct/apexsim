# apexsim-server

The authoritative ApexSim server: a Rust (tokio) crate that runs the race
simulation at 420 Hz, hosts the lobby and sessions over TCP, streams
telemetry and takes input over UDP, and serves health, metrics and an admin
dashboard over HTTP. The Unreal client in `../game-unreal` only draws what
this sends. The crate also builds `apexsim-replay` (headless races, showcase
renders, track guides, replay tools) and `convert_track` (CSV centerlines to
track YAML).

## Layout of `src/`

- **Entry and assembly**: `main.rs` (CLI, tracing, shutdown), `server.rs`
  (`run_server`, `ServerState`), `config.rs`, `lib.rs`.
- **Loop and IO**: `game_loop/` (`mod.rs` tick loop, `dispatch.rs`,
  `tick.rs`, `broadcast.rs`, `lifecycle.rs`, `track_loads.rs`,
  `showcase.rs`), `transport.rs`, `network.rs` (messages),
  `timer_resolution.rs`, `health.rs`, `metrics.rs`, `admin/` (dashboard).
- **Sessions and rules**: `game_session.rs`, `lobby.rs`, `data.rs` (core
  types), `laps.rs`, `records.rs`, `grid_order.rs`, `drs.rs`,
  `headlights.rs`, `debug_hooks.rs`.
- **Vehicle physics**: `physics.rs` (4-wheel model, collisions, surfaces),
  `tyre_thermal.rs`, `aero.rs`, `brakes.rs`, `engine_heat.rs`, `damage.rs`,
  `geometry.rs`, `hybrid.rs`, `slipstream.rs`, `feedback.rs` (force
  feedback), `car_setup.rs`, `setup_sheet.rs`.
- **AI**: `ai_driver.rs`, `racecraft.rs`, `racing_line.rs`, `pit.rs`.
- **Conditions and road**: `conditions.rs` (live sky), `road_state.rs`
  (rubber, marbles, water), `wind.rs`.
- **Content**: `car_loader.rs`, `track_loader.rs`, `track_content.rs` (lazy
  sidecar loading), `content_crc.rs`, and the sidecar readers `ground.rs`,
  `curbs.rs`, `walls.rs`, `road_mesh.rs`. Legacy: `procgen/` (terrain for
  `--generate-terrain`) and `track_mesh.rs` (used only by
  `examples/track_export.rs`).
- **Spectating and offline tools**: `spectator.rs`, `showcase.rs`,
  `replay.rs`, `replay_tools.rs`, `track_guide.rs`; binaries in `bin/`
  (`apexsim_replay.rs`, `convert_track.rs`).

Also here: `tests/` (integration tests; they start the server in-process, see
`tests/common/mod.rs`), `benches/physics_tick.rs` (Criterion), `examples/`,
the Docker files and three config files (`server.toml` for development,
`server.docker.toml` for the image; the release ships the repo root's).

## Build, test, run

```bash
cargo build --release
cargo fmt && cargo clippy --all-targets      # CI: fmt --check, clippy -D warnings
cargo test                                   # no running server needed
cargo test -- --ignored                      # long surveys, probes, stress
cargo bench
cargo run --release -- --config server.toml --log-level debug
```

While a server is running on Windows its exe is locked; build into another
`CARGO_TARGET_DIR` instead of stopping it.

## Further reading

- [server/CMDLINE.md](CMDLINE.md) - every command-line option and
  environment variable, and the `apexsim-replay` commands.
- [docs/server/operations.md](../docs/server/operations.md) - config,
  ports, TLS, health and metrics, the dashboard, Docker.
- [docs/server/protocol.md](../docs/server/protocol.md) - transport,
  messages, compatibility rules, golden bytes.
- [docs/architecture.md](../docs/architecture.md) - how the server, client
  and track pipeline fit together; determinism and tick timing.
- [docs/building.md](../docs/building.md) - building everything else.
