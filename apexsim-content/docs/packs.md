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

## Loose packs (.acp)

An `.acp` ("ApexSim content pack") is a plain zip of a player's own
content, made by hand or by a tool, for sharing: no manifest, no hashes,
any number of cars, tracks and HUD components. What it holds is worked
out from its files (`apexsim-lib/formats/src/pack/acp.rs`, `classify`),
and it installs into the `custom/` folders, never over shipped content.

| Item | Found by | Takes | Named |
|---|---|---|---|
| car | a folder holding `car.toml` | the whole folder (models, liveries, logos) | the folder; the pack's file name when `car.toml` is at the top |
| HUD component | a folder holding `component.json` | the whole folder | as a car |
| track | `<Stem>.yaml` with `<Stem>.uescene.json` anywhere in the zip | server half: every `<Stem>.*` file beside the YAML (sidecars, `.ats`, guide, layout); export: `<Stem>.uescene.json`, `<Stem>.uemesh`, `<Stem>.png` and `<Stem>.textures/`, wherever they sit | the stem |

So both of these are one track:

```
Ring.acp                          Ring.acp
  Ring/Ring.yaml                    Ring.yaml
  Ring/Ring.ground.msgpack ...      Ring.ground.msgpack ...
  game/Ring.uescene.json            Ring.uescene.json
  game/Ring.uemesh                  Ring.uemesh
  game/Ring.textures/...
```

A file in a folder item nested in another (a component inside a car folder)
belongs to the deeper one; a file in no item is listed as ignored, and
`__MACOSX/`, `.DS_Store`, `Thumbs.db` and `desktop.ini` are dropped.
A track is baked before it is packed (a release has no pipeline): its
`.uemesh` is required, a missing one of the five sidecars (`ground`,
`curbs`, `walls`, `road`, `pit`) is a warning on that track.

A whole pack is refused for a path that is absolute or climbs out with
`..`, two items of one kind and name, and no item at all. One item cannot
be installed, while the rest of the pack can, when its name is not
letters, digits, `-` and `_` (or starts with `_`), a track has no
`.uemesh`, a track is named as a shipped track, or a car `id` or
`track_id` is missing or already held by another item (both sides would
ignore it).

`apexsim-server content install <file>.acp` installs one, into a release
(`--root`, by default the server's own) or a checkout (`--checkout
<apexsim>`): every item, or only those named by `--only <kind>:<name>`
(`car:my-gt3`, `track:Ring`, `hud:gauge`, repeated). Installing an item
that cannot be installed stops the install before anything is written.
`--dry-run` lists every item with what it would replace, its warnings and,
for one that cannot be installed, why. The launcher's Manage content >
Install pack... shows that list with a tick box per item (those that cannot
be installed unticked and locked) and installs the ticked ones. Where
each item goes is in [../../apexsim/MODDING.md](../../apexsim/MODDING.md)
("Where things go"): an item replaces the player's own of its name (its
folders emptied first), and a HUD component named as a shipped one
replaces that on screen. Nothing is recorded in `installed.json`.

A release ships one: `content\content.acp`, the default cars and circuits
outside the starter set (`apexsim/scripts/build_release.ps1`,
[../../apexsim/docs/building.md](../../apexsim/docs/building.md#release-package)).
