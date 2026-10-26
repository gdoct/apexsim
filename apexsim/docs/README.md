# ApexSim documentation

These are the docs of the code root, `apexsim/`. The content root,
`apexsim-content/`, sits beside it and keeps its own docs in
`apexsim-content/docs/`. Paths in these docs are relative to the code root;
a path that starts with `apexsim-content/` is in the content root.

Each doc describes what is built, where its code is, how to check it, and
its traps. What is *not* built lives in the three backlogs. The `.html` pages
and `assets/` in this folder are the generated marketing site (GitHub Pages);
see [marketing-site.md](marketing-site.md).

## Start here

- [architecture.md](architecture.md): the server, client and track pipeline and how they talk; determinism, tick timing, wire-format rules, coordinates, content layout and checksums.
- [building.md](building.md): building and testing everything, a fresh checkout's generated content, playing from the editor build, packaging a release.

## Backlogs

- [simulation-gaps.md](simulation-gaps.md): what the simulation and the racing rules still leave out.
- [roadmap.md](roadmap.md): product features, content and tooling not started yet.
- [follow-ups.md](follow-ups.md): what is left on built features: bugs, edge cases, cleanup.
- [verification.md](verification.md): built features nobody has seen in the game, heard or felt on hardware yet: how they are checked; the items are in `verification/verification-items.yml`.
- [verifier.md](verifier.md): the verification backlog's file, its review page and the CLI the agent uses on it.

## Server (`server/server/`)

- [server/operations.md](server/operations.md): running a server: config, ports, TLS, env overrides, admin dashboard, health and metrics, Docker.
- [server/protocol.md](server/protocol.md): TCP/UDP transport, handshake, messages, positional telemetry, bounded queues, golden bytes.
- [server/sessions.md](server/sessions.md): session kinds and game modes, race start and finish, timed races, starting order and qualifying, lap timing and records, hotlap, session rules.
- [server/vehicle-physics.md](server/vehicle-physics.md): the car model: tyres, fuel, aero, brakes, engine heat, damage, suspension geometry, hybrid, slipstream, DRS, car setup.
- [server/conditions.md](server/conditions.md): weather, time of day, air and wind, the live sky, the road state.
- [server/ai.md](server/ai.md): AI drivers, racecraft, pit and fuel strategy, the AI survey.
- [server/pit-lane.md](server/pit-lane.md): the pit lane, autopilot, service and exit light.

## Unreal client (`game-unreal/`)

- [game/client.md](game/client.md): modules, menus and navigation, `settings.yml`, splash hold, triple monitors, car motion, racing-line overlay, unattended runs and debug switches.
- [game/cameras.md](game/cameras.md): cockpit rig, chase ladder, screenshot camera, TV director, demo backdrop.
- [game/audio.md](game/audio.md): synthesised engine, road and tyre sound, menu cues.
- [game/input-and-ffb.md](game/input-and-ffb.md): Enhanced Input, DirectInput wheels and pedals, force feedback.
- [game/hud-modding.md](game/hud-modding.md): HUD components and every data point (the modding reference).
- [game/telemetry-output.md](game/telemetry-output.md): the UDP packet for SimHub, motion rigs, shakers and dash apps (`settings.yml` `telemetry_out`).
- [game/spectator.md](game/spectator.md): spectator streams, showcases, the menu backdrop, watching races, replays and replay tools.

## Content (`apexsim-content/`: `docs/`, `track-editor/`, `tools/`)

- [track-pipeline.md](../../apexsim-content/docs/track-pipeline.md): how a circuit is generated, from dossier to export and sidecars, and how the client builds it at runtime.
- [track-format.md](../../apexsim-content/docs/track-format.md): track YAML, `.ats` scenes and the export format.
- [road-mesh.md](../../apexsim-content/docs/road-mesh.md): the road mesh sidecar and mesh wheel contact.
- [circuits.md](../../apexsim-content/docs/circuits.md): circuit styles and per-circuit notes (Nordschleife, Mandarina Bay).
- [naming.md](../../apexsim-content/docs/naming.md): no trademarks on screen: display names for tracks, corners and classes.
- [track-guide.md](../../apexsim-content/docs/track-guide.md): the corner-by-corner track guide.
- [cars.md](../../apexsim-content/docs/cars.md): car folders and car.toml, generated models, runtime loading, wheels, liveries, damage parts.
- [props.md](../../apexsim-content/docs/props.md): the trackside prop kit and its loader.
- [ac-import.md](../../apexsim-content/docs/ac-import.md): importing Assetto Corsa tracks and cars.

## Elsewhere

- [marketing-site.md](marketing-site.md): the generated site, in-engine shots and the promo video.
- [cota-startfinish-comparison.md](cota-startfinish-comparison.md): a broadcast onboard frame against the game's at the Circuit of the Americas' start/finish, region by region; the source of roadmap packages 39 to 50.
- [proposals/](proposals/): designs that are not built ([client-side prediction](proposals/client-prediction.md), [Nanite and shadows](proposals/nanite-shadows.md), [road state and snow](proposals/roadstate.md), [smaller recorded sessions](proposals/replay-size.md), [split-off content](proposals/split-off-content.md), [a learned AI driver](proposals/learned-ai-driver.md)).
- [../MODDING.md](../MODDING.md): the player-facing modding guide; [../server/server/CMDLINE.md](../server/server/CMDLINE.md): server command line.
