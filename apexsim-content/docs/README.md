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
code; the one exception is track-core's dev-dependency on the server crate
for its compat tests.

- [track-pipeline.md](track-pipeline.md): how a circuit is generated, from dossier to export and sidecars, and how the client builds it at runtime.
- [track-format.md](track-format.md): track YAML, `.ats` scenes and the export format.
- [road-mesh.md](road-mesh.md): the road mesh sidecar and mesh wheel contact.
- [circuits.md](circuits.md): circuit styles and per-circuit notes (Nordschleife, Mandarina Bay).
- [naming.md](naming.md): no trademarks on screen: display names for tracks, corners and classes.
- [track-guide.md](track-guide.md): the corner-by-corner track guide.
- [cars.md](cars.md): car folders and car.toml, generated models, runtime loading, wheels, liveries, damage parts.
- [props.md](props.md): the trackside prop kit and its loader.
- [ac-import.md](ac-import.md): importing Assetto Corsa tracks and cars.
