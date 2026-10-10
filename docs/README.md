# ApexSim documentation

Each doc describes what is built, where its code is, how to check it, and
its traps. What is *not* built lives in the two backlogs. `index.html` and
`assets/` in this folder are the generated marketing page (GitHub Pages);
see [marketing-site.md](marketing-site.md).

## Start here

- [architecture.md](architecture.md): the server, client and track pipeline and how they talk; determinism, tick timing, wire-format rules, coordinates, content layout and checksums.
- [building.md](building.md): building and testing everything, a fresh checkout's generated content, playing from the editor build, packaging a release.

## Backlogs

- [simulation-gaps.md](simulation-gaps.md): what the simulation and the racing rules still leave out.
- [roadmap.md](roadmap.md): product bugs, features, content and tooling not yet done.

## Server (`server/`)

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
- [game/spectator.md](game/spectator.md): spectator streams, showcases, the menu backdrop, watching races, replays and replay tools.

## Content (`content/`, `track-editor/`, `scripts/`)

- [content/track-pipeline.md](content/track-pipeline.md): how a circuit is generated, from dossier to export and sidecars, and how the client builds it at runtime.
- [content/track-format.md](content/track-format.md): track YAML, `.ats` scenes and the export format.
- [content/road-mesh.md](content/road-mesh.md): the road mesh sidecar and mesh wheel contact.
- [content/circuits.md](content/circuits.md): circuit styles and per-circuit notes (Nordschleife, Mandarina Bay).
- [content/naming.md](content/naming.md): no trademarks on screen: display names for tracks, corners and classes.
- [content/track-guide.md](content/track-guide.md): the corner-by-corner track guide.
- [content/cars.md](content/cars.md): car folders and car.toml, generated models, runtime loading, wheels, liveries, damage parts.
- [content/props.md](content/props.md): the trackside prop kit and its loader.
- [content/ac-import.md](content/ac-import.md): importing Assetto Corsa tracks and cars.

## Elsewhere

- [marketing-site.md](marketing-site.md): the generated site, in-engine shots and the promo video.
- [proposals/](proposals/): designs that are not built ([client-side prediction](proposals/client-prediction.md), [Nanite and shadows](proposals/nanite-shadows.md)).
- [../MODDING.md](../MODDING.md): the player-facing modding guide; [../server/CMDLINE.md](../server/CMDLINE.md): server command line.
