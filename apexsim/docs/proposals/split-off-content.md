# Split off the content

Status: phase 0 under way: the two roots are in place (2026-10-19); the rest is a proposal, not built.

Today one repository holds the simulation (`server/`), the client
(`game-unreal/`), the pipeline (`track-editor/`, `scripts/`) and every car,
circuit and prop kit (`content/`), and one zip holds everything a player
installs. The code is a few megabytes; the content is a gigabyte in LFS and
over four on disk once baked, and it changes on a different clock than the
code. This proposal moves the content into packs that are versioned, built
and installed on their own, keeps one car and two circuits in the main
repository so it builds and tests alone, and gives the launcher, the server
and the game an upgrade path in which a code release and a content release
are separate events that can be taken in either order.

## What is wrong with one repository

Measured on the checkout this was written on:

| | |
|---|---|
| Git packs | 106 MB |
| LFS objects (1318 files) | 942 MB |
| `content/` on disk | 1.2 GB (props 722 MB, tracks 267 MB, cars 157 MB) |
| `build/tracks` after a full bake | 4.1 GB |
| `game-unreal/Content` after `initialize_content.ps1` | 285 MB |

- **A clone is an LFS download.** A contributor who wants to fix a physics
  bug pulls every grandstand GLB and every `.blend` source first; CI does
  not (it has no sidecars, which is why the Le Mans test is skipped there).
- **Every release is a full release.** A corrected Spa pit lane or a new car
  ships as a new `ApexSim-<Version>-Win64.zip` with the client and the server
  in it, and a client fix ships every track again. A player keeps nothing
  between versions; a server operator stops the server to unzip.
- **The content is the content checksum's blind spot.** The CRC-32 says a
  player's `car.toml` differs from the server's and nothing more: not which
  version either side has, nor where the right one comes from.
- **Nothing says what fits together.** A track export only loads on a client
  that understands its `.uescene.json` `format`; a `car.toml` key only does
  something on a server that reads it. Those rules exist, but only in the
  code and the docs; no file states them.

## Shape

The rule that shapes everything below: **the code depends on the content,
never the other way round.** The server, the client, the launcher and the
release build read cars and circuits; nothing that authors, bakes or checks
content reads, builds or links the code. The tools that make content (the
track editor and `track-core`, the OSM, DEM and Assetto Corsa importers,
the Blender generators) therefore go with the content, and the formats the
two sides share are owned by the content side.

Two repositories and two kinds of artefact, plus a player's own folder.

| Repository | Holds | Produces |
|---|---|---|
| `apexsim` | server, client, launcher, release and deploy scripts, the site, the verify and promo tools, the default HUD, `showcase.yml`, the code docs | `ApexSim-<version>-Win64.zip`: launcher, `Game/`, `Server/`, and the **starter set** copied from the content |
| `apexsim-content` | cars, tracks, props, textures, wheels, artwork, the `formats` crate, `track-editor/` (editor, `track-core`, `ats-*`), the content tools, `starter.yml`, the content docs | the baked tree (exports, sidecars, previews, catalog), later **content packs** and a signed **pack index** |
| a player's `custom/` (never ours) | what `content/{cars,tracks}/custom` holds today | nothing published |

The split happens first inside this repository (phase 0). The root holds
two folders, `apexsim/` and `apexsim-content/`, each laid out as its
future repository, so the later split is a `git filter-repo
--subdirectory-filter` per folder and nothing moves twice. Until then, one
commit can still change both sides (a wire or format change).

### Phase 0: one repository, two roots

The two roots are in place. The tree, with what is still to come marked:

```
<repo>/
  CLAUDE.md, README.md, LICENSE, .gitattributes, .gitignore, .github/, .claude/, .vscode/
  apexsim/
    server/ game-unreal/ launcher/ deploy/ site/ docs/ server.toml (the release config)
    hud/default/                the default HUD (its components name the client's data points)
    showcase.yml                the menu showcases (rendered by the server's sim)
    scripts/                    build_*, initialize_content, import_cars, play_editor, deploy_server,
                                clean_workspace, verify/, promo/, site/, benchmark.py, motec_export.py,
                                lib/ApexEngine, ApexPhysicsLib, ApexShowcase, ApexGuide
    build/                      showcase/, guide/, benchmark/ (code artefacts)
    artifacts/ out/ records/ replays/
  apexsim-content/
    cars/ tracks/ props/ textures/ wheels/ artwork/
    track-editor/               Bevy editor, core/ (track-core, ats-*)
    tools/                      osm_*, dem_*, ac_import*, ac_car_import*, build_track_catalog, previews,
                                drs_zones, race line, check_walls, trace_layout, track_dirs.py,
                                lib/ApexTracks.ps1, lib/ApexCars.ps1, generators/ (Blender and texture generators)
    build/tracks/               the baked tree: exports, previews, catalog
    docs/                       the content docs
    .cache/                     osm, dem, previews, importer notices
    starter.yml                 (to come) the cars and tracks a release ships
    formats/                    (to come) Rust crate: track YAML, the five sidecars, content_crc
    bake.ps1                    (to come) dress, export, catalog, previews, ground textures
```

One CLAUDE.md at the repository root holds the rules for both roots. Each
root gets its own when the repositories split (phase 2).

**How the code finds the content.** The code reaches the content as
`../apexsim-content` beside the code root, the same before and after the
real split (two clones side by side). The server has `[content] root` in
`server.toml` and `APEXSIM_CONTENT` in the environment; `cars_dir` and
`tracks_dir` default to its `cars` and `tracks` and still override it. The
client has `-ApexContentRoot=` (`ApexRepoPaths.h`), with `-ApexTracksDir=`,
`-ApexCarsDir=` and `-ApexContentDir=` still overriding it. The code scripts
set `$ContentRoot` from their own folder. The content tools work from the
content root: the Python tools find it from their own file, and the
`ats-*` tools and the editor take paths relative to the working directory,
so callers run them from `apexsim-content/`. A release still lays out
`Game/` and `Server/` as before.

**Where the mixed things go.** Most tools clearly belong to one side. These
do not, so the rule places them:

| Thing | Goes to | Why |
|---|---|---|
| showcases (`apexsim-replay render`), `showcase.yml` | `apexsim` | a showcase is the server's sim run on a track: a physics change makes it stale, a content change only names the input. Every showcase is on a starter track, or a fresh install's menu has no backdrop. |
| track guides | the notes (`<Stem>.guide.yml`) stay with the track; the render (`apexsim-replay guide`) is `apexsim` | same reason. N28 moves both into the game, which removes the bake stage. |
| the default HUD | `apexsim/hud` | its JSON names data points the client defines ([../game/hud-modding.md](../game/hud-modding.md)); in the content it would depend on the code. A player's `hud/custom` sits beside it. |
| UE imports (`ApexPropImport`, `ApexCarImport`, `ApexGroundTexImport`, `ApexMaterialBake`, `ApexTrackImport`, `ApexTrackCatalogSync`) | `apexsim` | the client reading content, from the content root. `DT_CarCatalog`, `DT_TrackCatalog` and `UI/TrackPreviews` are generated from it. |
| the duplicated formats (track YAML, the road, ground, curbs, walls and pit sidecars, `content_crc`) | `apexsim-content/formats`, a crate the server and `track-core` both depend on | today each is written twice and kept in step by `track-core`'s dev-dependency on the server (`core/tests/compat.rs`, `road_mesh.rs`): a content-on-code edge. The C++ readers (`ApexCarToml.h`, `ApexContentCrc.h`, the scene reader) stay code, checked against the crate's golden files. |
| the AI survey (`survey_ai_races_on_every_circuit`) | `apexsim`, run against a content working copy | the acceptance check for wall, centerline and ground edits needs the sim. It is a check run from the code side, not a build dependency of the content. |
| the AC importers | `apexsim-content/tools` | pure Python over the content formats; the release copies them in as `Tools/importer`. |
| the site (`site/`, `scripts/site`, `docs/index.html`) | `apexsim` | it reads both sides (`facts.py` reads `server/src/car_setup.rs`; the shots need the server and the game). `artwork/` and `props/_preview` stay content. GitHub Pages deploys `apexsim/docs` through `.github/workflows/pages.yml`. |

**The starter set.** `apexsim-content/starter.yml` lists the cars and
tracks a release ships (decided 2026-10-18: one car per class, GT3, LMP2,
F1, Hypercar and Road, and Spa, Zandvoort and Monza). `build_release.ps1`
copies those from the baked tree; `-AllContent` ships everything, as now.
Because a code checkout always has the whole content root beside it, the
tests no longer have to fit a seed: the Norisring, Le Mans and Silverstone
tests stay as they are. Only what a fresh install shows has to be in the
starter set: the menu showcases, the demo backdrop and the first-run
defaults.

**Work left in phase 0.** Each step is one commit, and every test passes
after each:

1. **Split the orchestrators.** `build_track_levels.ps1` and
   `initialize_content.ps1` each do a content half (dress, export,
   catalog, previews, ground PNGs) and a code half (UE imports, material
   bake, showcases, guides, physics lib). The content half becomes
   `apexsim-content/bake.ps1`, which needs only cargo and Python; the code
   scripts call it when the baked tree is missing.
2. **The starter set.** `starter.yml`; `build_release.ps1` copying it; a
   preflight that every showcase and the demo backdrop are on starter
   tracks; and the client's backdrop tests (Austin today) moved onto
   starter tracks. Check: a release package built and raced from the zip,
   with the menu backdrop running.
3. **The formats crate.** `apexsim-content/formats` holds the track YAML,
   the five sidecars and `content_crc`. The server and `track-core` use it;
   their copies and `track-core`'s dev-dependency on the server go, and
   the compat tests become the server's own. Determinism and the golden
   bytes must not move. The YAML stops being modelled twice, and the
   CLAUDE.md rule about `TrackFileFormat` and `TrackFile` goes with it.
   `tools/ac_car_import/tests` reads `apexsim/server/src/brakes.rs` for
   its brake constants (and skips without it): that edge goes too.
4. **The direction checked in CI.** CI runs the server, the physics
   library, the track editor and the site from their roots; what is left is
   path filters per root and a check that nothing under `apexsim-content/`
   names `../apexsim`, `apexsim-server` or the code's scripts.
5. **Baked output out of the source folders** (optional, before packs).
   `ats-export` writes the sidecars into
   `apexsim-content/build/tracks/<Stem>/`, beside the export and a copy of
   the YAML, instead of gitignored files beside the source, and the
   server's tracks dir points at the baked tree. That folder is the
   `server/` half of a pack, so phase 1 only has to zip it.

### A pack

A pack is a zip of one content folder as it is checked in, plus the
generated files the two sides read, plus a manifest:

```
Spa-2026.10.3.apxpack
  pack.json
  server/Spa.yaml, Spa.ats, Spa.layout.json, Spa.{ground,curbs,walls,road,pit,dem}.msgpack, Spa.guide.yml
  game/Spa.uescene.json, Spa.uemesh, Spa.png
```

```
posh-gt3rs-2026.10.3.apxpack
  pack.json
  server/car.toml
  game/car.toml, posh_gt3rs.glb, posh_gt3rs_driver.glb, textures/...
```

`server/` is what `Server/content/...` holds in a release today and
`game/` what `Game/Tracks` or `Game/Cars` holds; the pack is the release
assembly of `build_release.ps1` cut per item. Showcases and rendered
guides are not in a pack: they are the code's (see the table above). The
`.blend` sources and the
raw OSM and DEM caches are not in a pack: they are in the content repository
for whoever rebuilds the pack.

`pack.json` (values illustrative):

```json
{
  "pack_format": 1,
  "kind": "track",
  "id": "b8edab00-8392-4765-967e-3c060828f922",
  "stem": "Spa",
  "display_name": "Spa-Francorchamps",
  "version": "2026.10.3",
  "content_crc": 2374481145,
  "formats": { "track_yaml": 1, "sidecars": 1, "scene": 3 },
  "requires": { "props": ">=2026.9" },
  "built_from": { "repo": "apexsim-content", "commit": "a1b2c3d" },
  "files": { "server/Spa.yaml": "sha256:...", "game/Spa.uemesh": "sha256:..." }
}
```

- `id` is the track's `track_id` or the car's `id`: the key every record,
  catalog row and showcase already uses. A pack never changes it.
- `content_crc` is the CRC-32 the server sends as `ContentCrc` and the
  export carries as `SourceCrc`, computed by the same `content_crc` rule. It
  is what the two sides compare at runtime; `version` is what people read.
- `formats` is the compatibility statement: the level of each format the
  pack's files are written in (for a car pack, `car_toml`). A pack never
  names a code version; the content does not know the code exists. Each
  level is a number owned by the `formats` crate (`scene` is the export's
  `format`, [apexsim-content/docs/track-format.md](../../../apexsim-content/docs/track-format.md)).
  `requires` is only for content on content: `props` is the props pack a
  track's `.ats` was dressed against.
- The code states what it reads: every build of the server and the game
  carries, from the `formats` crate it was built with, the range of levels
  it reads of each format (the server `track_yaml`, `sidecars` and
  `car_toml`; the game `scene` and `car_toml`). A reader refuses a pack
  whose levels fall outside its ranges, naming the format, before touching
  the install.
- `files` lets the launcher verify an install and repair one file.

### The index

`apexsim-content` publishes `index.json` with every pack's `id`, `kind`,
`stem`, `display_name`, class, latest `version`, `content_crc`, size,
`formats`, `requires` and download URL, with older versions listed under `history` so
a server pinned to an older pack can still be matched. The index is signed
(Ed25519, the public key in the launcher and the server), the packs are
named by hash in `files`, and nothing is installed that the index does not
vouch for. Where it is hosted is an operations choice (GitHub releases of
the content repository is enough); the URL is a launcher setting and a
`[content] index_url` key in `server.toml`, so a league can run its own
index.

### Where packs land

The release layout stays:

```
<install>/
  release.json                 code version, as today
  Game/Tracks/<Stem>...        from the pack's game/
  Game/Cars/<folder>/...
  Server/content/tracks/default/<Stem>/...   from the pack's server/
  Server/content/cars/default/<folder>/...
  Content/installed.json       which packs are installed, at which version and crc
  Content/packs/               the downloaded .apxpack files (kept for repair and roll back; prunable)
```

`installed.json` is the only new file the game and the server read, and
both read it only to report: the server keeps loading what is under
`[content]`, the game what is under `-ApexTracksDir=` and `-ApexCarsDir=`.
A player who drops a folder into `custom/` by hand sees it exactly as
today; it is simply not in `installed.json`.

The server reads `installed.json` to carry a `ContentVersion` beside
`ContentCrc` in the lobby's car and track summaries, and the game reads it
to put a version next to each catalog row. Both are `#[serde(default)]`
strings, left off the wire when empty, so an old client sees what it saw.

## Version rules

- **Code versions** the server and the game: one `ProjectVersion` for both
  (they are built from one commit and ship in one zip). The wire is still
  guarded by `PROTOCOL_VERSION`, which changes only when an old peer cannot
  survive. No pack names a code version.
- **A format level** changes only when a reader of the old level cannot
  read the new files. It belongs to the `formats` crate, and the code that
  reads the new level lands before (or with) the first pack written in it.
  The code index lists each code release's read ranges, so a launcher
  holding a pack it cannot read can name the release that can.
- **A pack version** is `<year>.<month>.<n>` and changes whenever any
  file in the pack changes. A YAML or `car.toml` edit changes `content_crc`
  as well; an export-only rebuild (new props, a mesh fix, a new preview)
  changes the version and not the crc, and the server does not care.
- **Compatibility is declared, not discovered.** A pack states the format
  levels it is written in; the code states the ranges it reads. The
  launcher and the server compare the two and refuse by the statement.
  Reading the pack and failing half way is the thing this replaces.
- **The content side stays append-only too.** A new `car.toml` key is
  neutral on a server that does not know it (CLAUDE.md, "a car feature is
  neutral without its keys"), so it does not raise the level, and a newer
  car pack runs on an older server. A new `.uescene.json` key is ignored by
  an older reader; only a change an old reader cannot survive raises a
  level, and that is refused.

## Upgrading the server

A server operator has `Server/` from the code zip and some set of packs. The
dashboard's Content view ([../server/operations.md](../server/operations.md))
grows an **Updates** section; everything it does is also the command line
`apexsim-server content {list,update,install,remove,verify}` so a Docker
host or a script can do it unattended.

**A new code version.** The operator unzips the new `Server/` over the old
one, or pulls the new image. On start the server reads `installed.json`,
checks every pack's `formats` against the ranges it reads and starts with the
packs that fit; a pack that does not is switched off the way the dashboard
switches content off today (`content.rs`), listed on the Content view with
the reason, and left on disk. Nothing is deleted by a code upgrade. If a
new server reads a sidecar format the pack is too old to carry, the pack
needs updating, and the view says so.

**A new pack version.** The server fetches the index (on demand from the
view, or every hour when `[content] auto_update` is on), lists packs with
a newer version than `installed.json` and shows the two crcs. Installing
one:

1. Download to `Content/packs/`, verify the hashes against the signed index.
2. Check `formats` against this server's read ranges, and `requires`
   against the installed packs; refuse before anything is replaced.
3. Unpack `server/` into a staging folder beside the live one.
4. Parse it as the loader would (`track_loader`, `CarConfig`), as the
   dashboard's upload does today; a file the server cannot read is refused
   and the staging folder removed.
5. Swap the folders, write `installed.json`.
6. Reload that one car or track into the catalog. A session already on it
   runs on with the copy it loaded (as a switched-off track does today);
   the next session on it gets the new one. The lobby's `ContentCrc` and
   `ContentVersion` change at that moment, so a client still on the old
   pack sees the mismatch toast on its next session, with the version it
   needs named in the toast rather than only "differs".

No restart is needed for a pack; one is needed for the code, through the
existing RESTART SERVER path. `auto_update` is off by default: a league
server should change content when its admin says, not when the index does.
A server can also be pinned (`[content] pin = ["Spa@2026.10.3"]`) so an
update lists it and never installs it.

**Docker.** `Dockerfile.content` goes away; the `/content` volume is
seeded by `apexsim-server content install --all --from <index>` on first
start and updated the same way. The image carries no content.

**Rolling back.** The previous pack is still in `Content/packs/`;
`content install Spa@<old>` is a downgrade through the same five steps.

## Upgrading the client

The launcher already checks the install and lists content; it becomes the
place a player updates both the code and the packs. It never does either
while the game or a local server is running (it checks for the processes
first, as the build scripts check for the editor).

**A new code version.** The launcher reads `release.json`, asks the code
index (the same signed index, with the code zip as a pack of kind `code`)
for the latest, and shows "Update to 0.4.1". Installing it:

1. Download, verify, unpack into `<install>/.staging/`.
2. Check every installed pack's `formats` against the new build's read
   ranges; list what will stop working, and let the player cancel or go on.
3. Move `Game/` and `Server/` aside (`<install>/.previous/`), move the new
   ones in, keep `Content/`, `settings.yml` (the game's, under its settings
   dir), `server.toml` and `custom/` untouched. A `settings.sample.yml`
   newer than the player's `settings.yml` is noted, never merged.
4. Write `release.json`; delete `.previous/` once the game has started once
   on the new build, or restore it from the launcher's "Roll back".

The launcher itself updates last, by renaming (`launcher.exe` cannot
replace itself while it runs; the new one is written as `launcher.new.exe`
and swapped by the next start).

**A new pack version.** The launcher's Manage content view lists every
installed pack with its version, the newest in the index, and whether it
fits this client (its `formats` against the game's read ranges); the track and car
pickers in the game get a small "update available" mark from
`installed.json` the same way, and a button that opens the launcher on that
pack. Installing a pack is the server's steps 1 to 5 run on `game/` into
`Game/Tracks` or `Game/Cars`, with no step 4 (the game builds a track the
first time it draws it, so the launcher only checks the manifest parses and
the files are complete). Because a player usually also runs a local server,
the launcher installs the pack's `server/` half into `Server/content/` in
the same step: one pack, both sides, always in step on one machine.

**Joining a server.** This is where the split pays off. Today the race
director compares crcs when it loads the track and shows a toast. With
packs:

1. The lobby's `SessionSummary` names the track, and the car and track
   summaries carry `ContentCrc` and `ContentVersion`.
2. Before joining, the game compares against `installed.json`. Same crc:
   join. Different crc and the server's version is in the index: the join
   button reads "Update Spa to 2026.10.3 and join", and the game asks the
   launcher (over a local pipe, the launcher is already there as the
   parent) to install it, then joins; the catalog rescans the way
   `apexsim.track.Rescan` does today, so no restart. Different crc and not
   in the index (the server runs its own edit, or a custom track): the toast
   as today, and the join still allowed, because spectating and racing on
   a slightly different road is what a modder may want.
3. A server can require a match (`[content] require_match = true`, echoed
   in `SessionJoined` as every rule is): then a mismatched client is told
   which pack version to install and the join is refused with `Error 403`,
   as a switched-off track is now.

Pack downloads come from the index, never from the game server: a game
server has no business serving a gigabyte of GLBs on its lobby port, and a
league's custom content goes in the league's own index.

**A fresh install** is the code zip with the starter set; the launcher's
first run offers "Install all content (1.1 GB)" or a pick by class and
circuit. A player who only ever races GT3 at three circuits downloads
those. The game's menus hide nothing: an uninstalled circuit shows in the
picker greyed with its size and installs from there.

## Building a pack

`build_packs.ps1` in the content repository, which is `bake.ps1` for one
item at a time and then a zip:

1. For a track: the refresh order of
   [apexsim-content/docs/track-pipeline.md](../../../apexsim-content/docs/track-pipeline.md) (dress,
   export, previews), then zip `server/` and `game/` with the manifest.
   `ats-export --strict` gates it.
2. For a car: check every file `car.toml` names is on disk, zip.
3. Version: `<year>.<month>.<n>` where `n` counts packs of that item built
   that month; the script refuses to build a pack whose files are identical
   to the latest published one.
4. `formats` from the `formats` crate the bake was built with, and
   `requires.props` from the props pack checked out.
5. Sign and append to `index.json`.

The content repository's CI bakes the calendar and runs `track-core`'s
tests and `ats-export --strict`; it never builds the code. The code
repository's CI checks out the content commit named in `content.lock`,
bakes it, and runs the server tests, the AI survey
(`survey_ai_races_on_every_circuit`) and the imported-content checks on
it, so a content change that breaks a circuit shows when the code moves its
lock, not on a player's machine. A content author judges a wall or
centerline edit the same way, by running the survey from a code checkout
against the content working copy.

## What changes in the code

- `server/src/content_crc.rs` stays. New `content_pack.rs`: read
  `pack.json` and `installed.json`, the `requires` check, the five install
  steps, `content {list,update,install,remove,verify}` on the command
  line, the dashboard's Updates section (`admin/content.rs`).
- `network.rs`: `ContentVersion` (`#[serde(default)]`) on the car and
  track summaries; `require_match` on `CreateSession`, echoed in
  `SessionJoined`; golden bytes on both sides in the same commit.
- `config.rs`: `[content] index_url`, `auto_update`, `pin`, `require_match`.
- Client: `UApexContentPackSubsystem` reads `installed.json`; the version
  on catalog rows; `VerifyTrackContent` / `VerifyCarContent` name the
  version; the join path's update-and-join; the pickers' greyed rows; the
  pipe to the launcher.
- Launcher: index fetch, signature check, download with resume, the
  install and roll-back of code and packs, the pipe server.
- `build_release.ps1` ships the starter set; `build_packs.ps1` is new;
  `Dockerfile.content` and `docker-compose.yml`'s content job go.
- Docs: `docs/content/packs.md` (the format and the index),
  `docs/server/operations.md` (updates on the dashboard),
  `docs/game/client.md` (the launcher), `docs/building.md` (two
  repositories), `MODDING.md` (a pack as the way to share a car).

## Phases

0. **One repository, two roots.** The work left above. Check: both
   roots build and test, a release is built from the starter set, and
   nothing under `apexsim-content/` refers to `apexsim/`.
1. **Pack format and reader, no network.** `pack.json`, `installed.json`,
   `build_packs.ps1`, `apexsim-server content install <file>` and the
   launcher installing a local `.apxpack`. The main repository still holds
   all content. Check: a Spa pack built from the checkout installs into a
   release package and the game races it with no mismatch toast.
2. **The split.** `apexsim` and `apexsim-content` each made with history
   by `git filter-repo --subdirectory-filter` on its root, keeping LFS; the
   code repository pins the content commit it was tested with
   (`content.lock`), and its CI checks that commit out beside it. Check:
   `cargo test` and the client tests pass on two clones side by side; the
   survey matches the figures from before the split.
3. **The index and updates.** Signing, the index, the dashboard's Updates,
   the launcher's update of packs and of the code, `ContentVersion` on the
   wire. Check: upgrade a release install from version N to N+1 and from
   pack version A to B both ways with the server running, then join a
   server on the other version in each combination of the table below.
4. **Update-and-join and `require_match`.** Check: a client with no Spa
   joins a Spa session with one click and races.

| Client has | Server has | Expected |
|---|---|---|
| pack A | pack A | joins, no toast |
| pack A | pack B (in index) | "update and join"; after it, no toast |
| pack B | pack A | "update and join" to A (a downgrade is an install) |
| pack A | a hand edit | toast as today; `require_match` refuses with the crc |
| code N, pack written at a format level N does not read | any | launcher refused the pack; picker shows it greyed with "needs ApexSim 0.4.1" (from the code index's read ranges) |
| code N+1 that no longer reads an old level | pack written at that old level | launcher refused it; it offers the pack's newer version |

## Not in this proposal

- Serving content from the game server, or peer to peer.
- A storefront, paid packs or DRM: the index is a signed list of files.
- Moving `game-unreal/Content` (menu, catalog tables, splash) anywhere: it
  is the client's, cooked into `Game/`.
- Linux or macOS launchers; the server's `content` command line covers a
  headless host.

## Traps

- **Two repositories, one checksum.** Until phase 0's formats crate (step 3) a track YAML
  is modelled in the server and in `track-core`; a key added to one and not
  the other drops out of a pipeline rewrite, and the content's bake then
  carries a different crc than the server computes. Do that step before the
  real split, so that both sides read through the one `formats` crate.
- **A pack version is not the crc.** Two versions can share a crc (export
  rebuilt, YAML untouched); the server matches on crc, people on version.
  Never make the server compare versions.
- **The starter set must carry a fresh install.** The tests run on the
  whole content root, but the menu showcases, the demo backdrop and the
  first-run defaults must name only starter tracks and cars; the release
  preflight checks it.
- **`custom/` is still the player's.** A pack installs only into
  `default/`; an uploaded custom car on a server stays a dashboard upload,
  not a pack, and never ships.
