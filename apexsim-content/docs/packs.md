# Content packs

A pack is one circuit or one car as a release ships it, in one file
(`<folder>-<version>.apxpack`), with a manifest that says what it is, what
each file's hash is and which format levels its files are written in. It is
how content will travel apart from the code
([../../apexsim/docs/proposals/split-off-content.md](../../apexsim/docs/proposals/split-off-content.md));
today (phase 1) packs are built from this checkout and installed into a
release package by hand. There is no index, signing or download yet.

## What is in one

A zip (deflate, fixed timestamps, so the same files make the same bytes):

```
SpaFrankenchamps-2026.10.1.apxpack
  pack.json
  server/tracks/default/SpaFrankenchamps/SpaFrankenchamps.yaml, .ground/.curbs/.walls/.road/.pit.msgpack, .guide.yml, .layout.json
  game/Tracks/SpaFrankenchamps.uescene.json, .uemesh, .png, SpaFrankenchamps.textures/...

posh-gt3rs-2026.10.1.apxpack
  pack.json
  server/cars/default/posh-gt3rs/car.toml
  game/Cars/default/posh-gt3rs/car.toml, the models, logos, skins and drivers it names
  game/Wheels/<class wheel>.glb, steering/<class wheel>.glb|json
```

`server/` mirrors a release's `Server/content/`, `game/` its `Game/`. A pack
may hold only its own item's paths (`PackManifest::may_hold`): its server
folder, its export (`game/Tracks/<Stem>.*`) or car folder, and for a car
the class wheels. The model is `apexsim-lib/formats/src/pack.rs`; reading
and writing the zip is its `archive` feature, shared by the builder and the
installer.

`pack.json`:

| Key | What |
|---|---|
| `pack_format` | 1 |
| `kind` | `track` or `car` |
| `id` | the YAML's `track_id` or the car's `id`; a pack never changes it |
| `folder` | the stem or the car folder |
| `display_name` | the YAML's `display_name` (or `name`), the car's `name` |
| `version` | `<year>.<month>.<n>`, what people read |
| `content_crc` | `content_crc` of the YAML or `car.toml`: what server and game compare |
| `formats` | the levels written: `track_yaml`, `sidecars`, `scene` for a circuit, `car_toml` for a car (`apexsim_formats::*_LEVEL`) |
| `requires`, `built_from` | content on content; the repository and commit |
| `files` | every other file with `sha256:<hex>` |

A reader refuses a pack whose levels fall outside what it reads
(`Reads::current`), naming the format, before it writes anything.

## Building one

From `apexsim-content/`, after the item's bake (the refresh order of
[track-pipeline.md](track-pipeline.md)):

```bash
cargo run --release --manifest-path track-editor/Cargo.toml -p track-core --bin ats-pack -- track SpaFrankenchamps
cargo run --release --manifest-path track-editor/Cargo.toml -p track-core --bin ats-pack -- car posh-gt3rs --out D:/packs
```

The pack goes to `build/packs/` (or `--out`). A circuit needs its four
baked sidecars and its export; a car every file its `car.toml` names and
the class wheels (the release's rules, `tools/lib/ApexCars.ps1`). `n` in
the version counts this month's packs of the item already in the output
folder, and a pack whose files are those of the latest there is not built
again (`--force` builds it anyway).

## Installing one

`Server\apexsim-server.exe content install <pack>` in a release package
(`--root` names another install), or the launcher's Manage content >
Install pack.... The pack is read whole and checked, the item's own
folders emptied, its files written where the server and the game already
look, the pack kept in `Content/packs/` and the install recorded in
`Content/installed.json`. `content list` prints that file, `content
verify` checks every installed file against its pack
([../../apexsim/server/server/CMDLINE.md](../../apexsim/server/server/CMDLINE.md#content)).
Nothing reads `installed.json` to decide what loads: the server loads its
content folders, the game what is beside it, as before.

A pack installs only into `default/`; `custom/` stays the player's.
