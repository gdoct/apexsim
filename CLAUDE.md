# CLAUDE.md

ApexSim is a sim-racing platform: an authoritative Rust server (`server/`,
420 Hz physics, TCP+TLS lobby, UDP telemetry and input), an Unreal Engine 5.8
client (`game-unreal/`) that renders what the server says and has no physics
of its own, and a Rust + Python content pipeline (`track-editor/`, `scripts/`)
that turns real circuits and cars in `content/` into what both sides load.

## Where to read first

Feature detail lives in `docs/`, one doc per area ([docs/README.md](docs/README.md)
is the index). Before changing an area, read its doc; it names the code, the
checks and the traps.

| Changing | Read |
|---|---|
| anything across server and client, the wire, determinism | [docs/architecture.md](docs/architecture.md), [docs/server/protocol.md](docs/server/protocol.md) |
| builds, tests, scripts, packaging, a fresh clone | [docs/building.md](docs/building.md) |
| server config, ports, TLS, dashboard | [docs/server/operations.md](docs/server/operations.md) |
| sessions, race rules, timing, qualifying, hotlap | [docs/server/sessions.md](docs/server/sessions.md) |
| car physics, tyres, fuel, aero, damage, setup | [docs/server/vehicle-physics.md](docs/server/vehicle-physics.md) |
| weather, sky, wind, road state | [docs/server/conditions.md](docs/server/conditions.md) |
| AI drivers, racecraft, strategy | [docs/server/ai.md](docs/server/ai.md) |
| pit lane and stops | [docs/server/pit-lane.md](docs/server/pit-lane.md) |
| client menus, settings, splash, motion, debug switches | [docs/game/client.md](docs/game/client.md) |
| cameras, TV director, demo backdrop | [docs/game/cameras.md](docs/game/cameras.md) |
| sound | [docs/game/audio.md](docs/game/audio.md) |
| input, wheels, force feedback | [docs/game/input-and-ffb.md](docs/game/input-and-ffb.md) |
| the HUD | [docs/game/hud-modding.md](docs/game/hud-modding.md) |
| spectating, showcases, replays | [docs/game/spectator.md](docs/game/spectator.md) |
| track generation, sidecars, runtime tracks | [docs/content/track-pipeline.md](docs/content/track-pipeline.md), [docs/content/track-format.md](docs/content/track-format.md), [docs/content/road-mesh.md](docs/content/road-mesh.md), [docs/content/circuits.md](docs/content/circuits.md) |
| cars, props | [docs/content/cars.md](docs/content/cars.md), [docs/content/props.md](docs/content/props.md) |
| Assetto Corsa import | [docs/content/ac-import.md](docs/content/ac-import.md) |
| track guide, marketing site, promo video | [docs/content/track-guide.md](docs/content/track-guide.md), [docs/marketing-site.md](docs/marketing-site.md) |

## Commands

```bash
cd server && cargo build && cargo test          # integration tests start the server in-process
cd server && cargo fmt && cargo clippy --all-targets   # CI: fmt --check, clippy -D warnings
cargo test --manifest-path track-editor/Cargo.toml     # pipeline + Bevy editor (whole-calendar bakes are #[ignore]d)
```

The client, the commandlets and the content scripts (`initialize_content.ps1`,
`play_editor.ps1`, `build_track_levels.ps1`, `build_release.ps1`) are in
[docs/building.md](docs/building.md).

## Rules for every change

- **Deterministic sim.** No RNG, wall clock or env reads in the sim path, and
  no `HashMap` iteration order reaching a result: participants are a
  `BTreeMap`, chance is `wind::hash01` of tick, car, lap or corner.
  `tests/determinism_test.rs` must stay bit-identical; showcases, guides,
  replays and site shots all rely on seeded runs replaying exactly.
- **The wire is append-only.** Named fields get `#[serde(default)]` and are
  left off the wire at their default; positional fields (`TelemetryCompact`,
  `DriverFeedback`, spectator rows) go at the end. A wire change lands on both
  sides in one commit with its golden bytes
  (`cargo test <name>_wire_format -- --nocapture` into the client's
  `ApexGoldenBlobs.h` / `ApexUdpGoldenBlobs.h`). A new session rule is a
  `CreateSession` field echoed in `SessionJoined`.
- **The server owns every rule.** Collisions, track limits, the pit lane,
  weather grip and assists exist only on the server; the client is a puppet of
  telemetry. Weather and air are baked into the session's own track copy:
  never branch on weather in the tick.
- **Never hard-code the tick rate**: use `dt` and `DEFAULT_TICK_RATE_HZ`. New
  per-tick track queries go through the cached windowed centerline search.
- **A car feature is neutral without its keys**: a car.toml that does not name
  a new table or key simulates exactly as before, and every feature is 1.0 at
  the car's reference state, or the class calibration and physics tests move.
- **Judge physics, wall, centerline and ground changes with the AI survey**
  (`survey_ai_races_on_every_circuit`) against the commit before, comparing
  totals. The survey and the other offline tools drive the centerline; the
  live server drives the road mesh: a surface change must hold on both.
- **Generated files are regenerated, not edited**: `docs/index.html` and
  `docs/assets/` (`scripts/site/build_site.py`), `build/tracks`, the track
  sidecars, `game-unreal/Content` beyond the menu and catalog tables. A track
  YAML or car.toml edit changes its content checksum: re-export the track,
  and showcases and guides built from it go stale.
- **Track data flows in the refresh order** in
  [docs/content/track-pipeline.md](docs/content/track-pipeline.md); run
  `ats-*` tools from the repo root. The track YAML is modelled twice
  (`server/src/track_loader.rs` `TrackFileFormat`, `track-editor/core/src/track_data.rs`
  `TrackFile`): a key added to one goes in the other, or a pipeline rewrite
  drops it. A track or car marked `imported` belongs
  to its importer: tools that rewrite content must leave it alone.
  `content/{cars,tracks}/custom` is the player's own and never ships.
- **No trademarks on screen.** A real circuit, corner, series or car name
  lives in `name`; screens show `display_name`, `CORNER_DISPLAY` or
  `ApexCatalog::DisplayClass` ([docs/content/naming.md](docs/content/naming.md)).
- **HUD data points are the modding API**: a new one is set on every path of
  `ApexHudData::Build` and listed in `docs/game/hud-modding.md`, or
  `ApexSim.Hud.Data.Stable` / `.Documented` fail.
- **Client traps.** Index enums that are also command-line values
  (`EApexScreen`, `EApexSettingsTab`) are append-only. While driving, focus
  stays on the game viewport, or Slate eats pad steering as menu navigation.
  A car's own material comes from `ApexCarContent::OwnMaterialInstance`. Check
  a new `.cpp` with `-DisableAdaptiveUnity`, and check behaviour unattended
  (`-ApexAutoRace`, `-ApexExecAfter`, `-ApexScreenshotAfter`;
  [docs/game/client.md](docs/game/client.md)).
- **A running server locks its exe on Windows**: build into a scratch
  `CARGO_TARGET_DIR` rather than stopping it.

## Keeping the docs

- A change updates its area's doc in the same commit. Write what *is*, in the
  present tense; history belongs in git, not in the doc.
- Anything left for later goes in [docs/simulation-gaps.md](docs/simulation-gaps.md)
  (simulation and racing rules) or [docs/roadmap.md](docs/roadmap.md)
  (product, UI, content, tooling); delete the row when it is built.
- This file holds only rules that bind changes anywhere in the repo. Feature
  detail goes in the topic doc, not here.
