# CLAUDE.md

ApexSim is a sim-racing platform in three roots that become three repositories.
`apexsim/` is the code: the deterministic simulation (`apexsim/sim/`, the
`apexsim-sim` crate: the car, the track, the race, the session tick), an
authoritative Rust server on it (`apexsim/server/`, 420 Hz, TCP+TLS lobby,
UDP telemetry and input), an Unreal Engine 5.8
client (`apexsim/game-unreal/`) that renders what the server says and has no
physics of its own, the HUD, the site and the build scripts (`apexsim/scripts/`).
`apexsim-content/` is the content and its tools: the cars, tracks, props and
textures, the Rust track pipeline and editor (`apexsim-content/track-editor/`)
and the Python importers and generators (`apexsim-content/tools/`) that turn
real circuits and cars into what both sides load. `apexsim-lib/` holds the
formats the two share: the `apexsim-formats` crate (`apexsim-lib/formats/`)
with the track YAML, the baked sidecars and `content_crc`.

`apexsim-lib` depends on nothing; the content depends on `apexsim-lib`; the
code depends on `apexsim-lib` and the content, never the other way round.
The code reaches the content as the sibling `../apexsim-content`, both build
against `../apexsim-lib` by path, and nothing under `apexsim-content/` or
`apexsim-lib/` reads, builds or links `apexsim/`. One test-only edge is left:
the AC car importer's test reading `apexsim/sim/src/brakes.rs`.

## Where to read first

Feature detail lives in `apexsim/docs/` and `apexsim-content/docs/`, one doc
per area ([apexsim/docs/README.md](apexsim/docs/README.md) is the index). Before changing an area, read its doc; it names the code, the
checks and the traps.

| Changing | Read |
|---|---|
| anything across server and client, the wire, determinism | [apexsim/docs/architecture.md](apexsim/docs/architecture.md), [apexsim/docs/server/protocol.md](apexsim/docs/server/protocol.md) |
| builds, tests, scripts, packaging, a fresh clone | [apexsim/docs/building.md](apexsim/docs/building.md) |
| server config, ports, TLS, dashboard | [apexsim/docs/server/operations.md](apexsim/docs/server/operations.md) |
| sessions, race rules, timing, qualifying, hotlap | [apexsim/docs/server/sessions.md](apexsim/docs/server/sessions.md) |
| car physics, tyres, fuel, aero, damage, setup | [apexsim/docs/server/vehicle-physics.md](apexsim/docs/server/vehicle-physics.md) |
| weather, sky, wind, road state | [apexsim/docs/server/conditions.md](apexsim/docs/server/conditions.md) |
| AI drivers, racecraft, strategy | [apexsim/docs/server/ai.md](apexsim/docs/server/ai.md) |
| pit lane and stops | [apexsim/docs/server/pit-lane.md](apexsim/docs/server/pit-lane.md) |
| client menus, settings, splash, motion, debug switches | [apexsim/docs/game/client.md](apexsim/docs/game/client.md) |
| cameras, TV director, demo backdrop | [apexsim/docs/game/cameras.md](apexsim/docs/game/cameras.md) |
| sound | [apexsim/docs/game/audio.md](apexsim/docs/game/audio.md) |
| input, wheels, force feedback | [apexsim/docs/game/input-and-ffb.md](apexsim/docs/game/input-and-ffb.md) |
| the HUD | [apexsim/docs/game/hud-modding.md](apexsim/docs/game/hud-modding.md) |
| spectating, showcases, replays | [apexsim/docs/game/spectator.md](apexsim/docs/game/spectator.md) |
| track generation, sidecars, runtime tracks | [apexsim-content/docs/track-pipeline.md](apexsim-content/docs/track-pipeline.md), [apexsim-content/docs/track-format.md](apexsim-content/docs/track-format.md), [apexsim-content/docs/road-mesh.md](apexsim-content/docs/road-mesh.md), [apexsim-content/docs/circuits.md](apexsim-content/docs/circuits.md) |
| cars, props | [apexsim-content/docs/cars.md](apexsim-content/docs/cars.md), [apexsim-content/docs/props.md](apexsim-content/docs/props.md) |
| Assetto Corsa import | [apexsim-content/docs/ac-import.md](apexsim-content/docs/ac-import.md) |
| track guide, marketing site, promo video | [apexsim-content/docs/track-guide.md](apexsim-content/docs/track-guide.md), [apexsim/docs/marketing-site.md](apexsim/docs/marketing-site.md) |

## Commands

```bash
cd apexsim-lib/formats && cargo test                    # the shared formats: YAML rewrite, sidecar goldens
cd apexsim/sim && cargo test                            # the simulation's unit tests
cd apexsim/server && cargo build && cargo test          # integration tests start the server in-process
cd apexsim/server && cargo fmt && cargo clippy --all-targets   # CI: fmt --check, clippy -D warnings
cargo test --manifest-path apexsim-content/track-editor/Cargo.toml   # pipeline + Bevy editor (whole-calendar bakes are #[ignore]d)
```

Code scripts run from `apexsim/`, content tools (`ats-*`, `tools/*.py`) from
`apexsim-content/`. The client, the commandlets and the scripts
(`initialize_content.ps1`, `play_editor.ps1`, `build_track_levels.ps1`,
`build_release.ps1` in `apexsim/scripts/`) are in
[apexsim/docs/building.md](apexsim/docs/building.md).

## Rules for every change

- **Deterministic sim.** No RNG, wall clock or env reads in the sim path
  (`apexsim/sim`, re-exported by the server under the same module paths), and
  no `HashMap` iteration order reaching a result: participants are a
  `BTreeMap`, chance is `wind::hash01` of tick, car, lap or corner.
  `apexsim/server/tests/determinism_test.rs` must stay bit-identical; showcases, guides,
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
- **Generated files are regenerated, not edited**: `apexsim/docs/index.html`
  and `apexsim/docs/assets/` (`apexsim/scripts/site/build_site.py`),
  `apexsim-content/build/tracks`, the track sidecars,
  `apexsim/game-unreal/Content` beyond the menu and catalog tables. A track
  YAML or car.toml edit changes its content checksum: re-export the track,
  and showcases and guides built from it go stale.
- **Track data flows in the refresh order** in
  [apexsim-content/docs/track-pipeline.md](apexsim-content/docs/track-pipeline.md); run
  `ats-*` tools from `apexsim-content/`. The track YAML and the sidecars
  are modelled once, in `apexsim-lib/formats` (`TrackFile`, the sidecar
  structs, their versions): a key goes there, and the simulation and
  track-core re-export it. Its serde attributes are what a rewrite writes, so a change
  to one moves every rewritten YAML's bytes and checksum. A track or car marked `imported` belongs
  to its importer: tools that rewrite content must leave it alone.
  `apexsim-content/{cars,tracks}/custom` is the player's own and never ships.
- **No trademarks on screen.** A real circuit, corner, series or car name
  lives in `name`; screens show `display_name`, `CORNER_DISPLAY` or
  `ApexCatalog::DisplayClass` ([apexsim-content/docs/naming.md](apexsim-content/docs/naming.md)).
- **HUD data points are the modding API**: a new one is set on every path of
  `ApexHudData::Build` and listed in `apexsim/docs/game/hud-modding.md`, or
  `ApexSim.Hud.Data.Stable` / `.Documented` fail.
- **Client traps.** Index enums that are also command-line values
  (`EApexScreen`, `EApexSettingsTab`) are append-only. While driving, focus
  stays on the game viewport, or Slate eats pad steering as menu navigation.
  A car's own material comes from `ApexCarContent::OwnMaterialInstance`. Check
  a new `.cpp` with `-DisableAdaptiveUnity`, and check behaviour unattended
  (`-ApexAutoRace`, `-ApexExecAfter`, `-ApexScreenshotAfter`;
  [apexsim/docs/game/client.md](apexsim/docs/game/client.md)).
- **A running server locks its exe on Windows**: build into a scratch
  `CARGO_TARGET_DIR` rather than stopping it.

## Keeping the docs

- A change updates its area's doc in the same commit. Write what *is*, in the
  present tense; history belongs in git, not in the doc.
- Anything left for later goes in [apexsim/docs/simulation-gaps.md](apexsim/docs/simulation-gaps.md)
  (simulation and racing rules), [apexsim/docs/follow-ups.md](apexsim/docs/follow-ups.md)
  (bugs, edge cases and cleanup left on a built feature),
  [apexsim/docs/verification.md](apexsim/docs/verification.md) (built but not yet seen in
  the game, heard or felt on hardware) or [apexsim/docs/roadmap.md](apexsim/docs/roadmap.md)
  (product, UI, content and tooling features not started); delete the row
  when it is done.
- Working through [apexsim/docs/work-plan.md](apexsim/docs/work-plan.md), commit after every
  item that is solved: its code, its doc, and its row deleted from the work
  plan and its backlog, in one commit, before the next item starts. Leave
  edits that are not part of the item out of it.
- This file holds only rules that bind changes anywhere in the repo. Feature
  detail goes in the topic doc, not here.
