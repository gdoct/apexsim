# ApexSim content documentation

These are the docs of the content root, `apexsim-content/`: the cars, tracks,
props and textures, and the tools that make them (`track-editor/` and its
`ats-*` tools, the Python tools in `tools/`). The code (server, client,
scripts) is the code root, `apexsim/`, beside it, with its own docs in
`apexsim/docs/` ([index](../../apexsim/docs/README.md)).

Paths in these docs are relative to the content root, and every command runs
from it (`python tools/<tool>.py`,
`cargo run --manifest-path track-editor/Cargo.toml --bin ats-<tool> -- ...`).
A path that starts with `apexsim/` is in the code root; a command reaches it
as `../apexsim/...`. Nothing in the content root reads, builds or links the
code; the one exception is the AC car importer's test reading the server's
brake constants. The formats the two share (the track YAML, the sidecars,
`content_crc`) are the `apexsim-formats` crate in the third root,
`apexsim-lib/formats`, which track-core builds against and which depends on
neither.

- [track-pipeline.md](track-pipeline.md): how a circuit is generated, from dossier to export and sidecars, and how the client builds it at runtime.
- [track-format.md](track-format.md): track YAML, `.ats` scenes and the export format.
- [road-mesh.md](road-mesh.md): the road mesh sidecar and mesh wheel contact.
- [circuits.md](circuits.md): circuit styles and per-circuit notes (Nordschleife, Mandarina Bay).
- [circuit-references/](circuit-references/README.md): the real venues the circuits are modelled on, at building level: layout history, corners, pits, grandstands, structures, maps.
- [naming.md](naming.md): no trademarks on screen: display names for tracks, corners and classes.
- [track-guide.md](track-guide.md): the corner-by-corner track guide.
- [cars.md](cars.md): car folders and car.toml, generated models, runtime loading, wheels, liveries, damage parts.
- [props.md](props.md): the trackside prop kit and its loader.
- [track-improvements.md](track-improvements.md): bringing every circuit's dressing up to its references: the method, the per-track status table, the kit gaps.
- [ac-import.md](ac-import.md): importing Assetto Corsa tracks and cars.
- [packs.md](packs.md): content packs: one circuit or car in one file, built with `ats-pack`, installed into a release.

`drivers/portraits/face_<n>.png` (1 to 16, 200 x 308 cut-outs on a clear
background, the drivers' suits in the parody sponsors of
[naming.md](naming.md)) are the pictures a driver profile picks from. The
client reads them at runtime, from here in the editor and from
`Drivers/portraits` beside a packaged game (`apexsim/scripts/build_release.ps1`
copies them); a new picture is the next number, and the game takes them up
to the first one missing ([client.md](../../apexsim/docs/game/client.md#driver-profiles-apexprofilesh-uiapexprofilewidgetcpp)).

`audio/radio/<id>.wav` are the spotter's and the race engineer's voice
lines, rendered from `audio/radio/lines.yml` (the id, the channel and the
words) by `python tools/generators/radio/build_radio_lines.py [ids]` with the
local Windows voices (System.Speech, offline), through a radio band, a
compressor, a faint hiss and a squelch at each end; 16 kHz mono, the same
bytes every run. A new line is a row in `lines.yml`, a render, and the
client trigger that says it. The client reads them from here in the editor
and from `Audio/radio` beside a packaged game
([audio.md](../../apexsim/docs/game/audio.md#radio)).
