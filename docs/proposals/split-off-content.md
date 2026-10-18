# Split off the content

Status: proposal, not built.

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

Three repositories and three kinds of artefact.

| Repository | Holds | Produces |
|---|---|---|
| `apexsim` (this one) | server, client, pipeline, launcher, docs, and a **seed**: one GT3 car (`posh-gt3rs`) and two circuits (`Spa`, `Zandvoort`) with their dossiers, DEM sidecars and `.ats`, enough for every test and showcase to run | `ApexSim-<version>-Win64.zip`: launcher, `Game/`, `Server/`, the seed content |
| `apexsim-content` | `content/cars/default`, `content/tracks/default`, `content/props`, `content/textures`, `content/wheels`, `content/artwork`, `content/showcase.yml`, the car generators' sources (`.blend`) | **content packs**, one per track and one per car, plus a `props` pack and a `wheels` pack, and a signed **pack index** |
| `apexsim-content-custom` (a player's, never ours) | what `content/{cars,tracks}/custom` holds today | nothing published |

The main repository's `content/` folder keeps its layout exactly; only the
set of folders under `default/` shrinks to the seed. Every tool that walks
`content/tracks/{default,custom}` walks the same paths; a developer who
wants the whole calendar clones `apexsim-content` beside the checkout and
points `[content]` and `-ApexTracksDir=` at it, or runs
`initialize_content.ps1 -Content ../apexsim-content`, which junctions its
`default/` folders in (a junction, never a copy; see the traps at the end
for how a junction bites).

### A pack

A pack is a zip of one content folder as it is checked in, plus the
generated files the two sides read, plus a manifest:

```
Spa-2026.10.3.apxpack
  pack.json
  server/Spa.yaml, Spa.ats, Spa.layout.json, Spa.{ground,curbs,walls,road,pit,dem}.msgpack, Spa.guide.yml
  game/Spa.uescene.json, Spa.uemesh, Spa.png
  showcase/Spa.apxs             (when content/showcase.yml renders one here)
  guide/Spa/...                 (the track guide, docs/content/track-guide.md)
```

```
posh-gt3rs-2026.10.3.apxpack
  pack.json
  server/car.toml
  game/car.toml, posh_gt3rs.glb, posh_gt3rs_driver.glb, textures/...
```

`server/` is what `Server/content/...` holds in a release today and
`game/` what `Game/Tracks` or `Game/Cars` holds; the pack is the release
assembly of `build_release.ps1` cut per item. The `.blend` sources and the
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
  "requires": { "server": ">=0.4", "game": ">=0.4", "scene_format": 3, "props": ">=2026.9" },
  "built_from": { "repo": "apexsim-content", "commit": "a1b2c3d" },
  "files": { "server/Spa.yaml": "sha256:...", "game/Spa.uemesh": "sha256:..." }
}
```

- `id` is the track's `track_id` or the car's `id`: the key every record,
  catalog row and showcase already uses. A pack never changes it.
- `content_crc` is the CRC-32 the server sends as `ContentCrc` and the
  export carries as `SourceCrc`, computed by the same `content_crc` rule. It
  is what the two sides compare at runtime; `version` is what people read.
- `requires` is the compatibility statement. `server` and `game` are
  ranges over the code's version (`ProjectVersion` in `DefaultGame.ini`,
  which the server's `Cargo.toml` is moved to share); `scene_format` is the
  export's `format` (`docs/content/track-format.md`); `props` is the props
  pack a track's `.ats` was dressed against. A reader refuses a pack whose
  requirements it does not meet, with the manifest's words, before touching
  the install.
- `files` lets the launcher verify an install and repair one file.

### The index

`apexsim-content` publishes `index.json` with every pack's `id`, `kind`,
`stem`, `display_name`, class, latest `version`, `content_crc`, size,
`requires` and download URL, with older versions listed under `history` so
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
  survive; the code version is what packs require.
- **A pack version** is `<year>.<month>.<n>` and changes whenever any
  file in the pack changes. A YAML or `car.toml` edit changes `content_crc`
  as well; an export-only rebuild (new props, a mesh fix, a new preview)
  changes the version and not the crc, and the server does not care.
- **Compatibility is declared, not discovered.** A pack states what it
  needs; the code states what it offers (`scene_format` it reads, pack
  formats it accepts). The launcher and the server refuse by the statement.
  Reading the pack and failing half way is the thing this replaces.
- **The content side stays append-only too.** A new `car.toml` key is
  neutral on a server that does not know it (CLAUDE.md, "a car feature is
  neutral without its keys"), so a newer car pack runs on an older server
  until a pack says otherwise in `requires.server`. A new `.uescene.json`
  key is ignored by an older reader; a new `format` number is refused.

## Upgrading the server

A server operator has `Server/` from the code zip and some set of packs. The
dashboard's Content view ([../server/operations.md](../server/operations.md))
grows an **Updates** section; everything it does is also the command line
`apexsim-server content {list,update,install,remove,verify}` so a Docker
host or a script can do it unattended.

**A new code version.** The operator unzips the new `Server/` over the old
one, or pulls the new image. On start the server reads `installed.json`,
checks every pack's `requires.server` against itself and starts with the
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
2. Check `requires` against this server; refuse before anything is replaced.
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
start and updated the same way. The image carries only the seed.

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
2. Check every installed pack's `requires.game` against the new version;
   list what will stop working, and let the player cancel or go on.
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
fits this client (`requires.game`, `scene_format`); the track and car
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

**A fresh install** is the code zip with the seed content; the launcher's
first run offers "Install all content (1.1 GB)" or a pick by class and
circuit. A player who only ever races GT3 at three circuits downloads
those. The game's menus hide nothing: an uninstalled circuit shows in the
picker greyed with its size and installs from there.

## Building a pack

`scripts/build_packs.ps1` in the content repository, which is
`build_release.ps1`'s stages 3, 4 and 6 for one item at a time:

1. For a track: the refresh order of
   [../content/track-pipeline.md](../content/track-pipeline.md) (dress,
   export, previews, the showcase named in `showcase.yml`, the guide), then
   zip `server/` and `game/` with the manifest. `ats-export --strict`
   gates it.
2. For a car: check every file `car.toml` names is on disk, zip.
3. Version: `<year>.<month>.<n>` where `n` counts packs of that item built
   that month; the script refuses to build a pack whose files are identical
   to the latest published one.
4. `requires` from the tool versions that built it: `scene_format` from
   `ats-export`, `props` from the props pack checked out, `server` and
   `game` from the `apexsim` tag the content repository's CI built
   against.
5. Sign and append to `index.json`.

The content repository's CI builds the seed-less calendar against the
latest `apexsim` tag and runs the AI survey
(`survey_ai_races_on_every_circuit`) and the imported-content checks
against the packs, so a pipeline change in `apexsim` that breaks a circuit
shows there, not on a player's machine. The main repository's CI runs the
server tests on the seed, which it has in full (sidecars included, so the
skipped Le Mans test becomes a Spa test that runs).

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
- `build_release.ps1` ships the seed only; `build_packs.ps1` is new;
  `initialize_content.ps1 -Content` junctions a content checkout;
  `Dockerfile.content` and `docker-compose.yml`'s content job go.
- Docs: `docs/content/packs.md` (the format and the index),
  `docs/server/operations.md` (updates on the dashboard),
  `docs/game/client.md` (the launcher), `docs/building.md` (two
  repositories), `MODDING.md` (a pack as the way to share a car).

## Phases

1. **Pack format and reader, no network.** `pack.json`, `installed.json`,
   `build_packs.ps1`, `apexsim-server content install <file>` and the
   launcher installing a local `.apxpack`. The main repository still holds
   all content. Check: a Spa pack built from the checkout installs into a
   release package and the game races it with no mismatch toast.
2. **The split.** `apexsim-content` created with history
   (`git filter-repo` on `content/`, keeping LFS), the seed left behind,
   `initialize_content.ps1 -Content`, CI on both sides. Check: `cargo test`
   and the client tests pass on a clone of `apexsim` alone; the survey runs
   from a content checkout and matches the figures before the split.
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
| code N, pack needing N+1 | any | launcher refused the pack; picker shows it greyed with "needs 0.4.1" |
| code N+1 | pack needing N (older `scene_format`) | the game refuses the export by `format`; launcher offers the pack's newer version |

## Not in this proposal

- Serving content from the game server, or peer to peer.
- A storefront, paid packs or DRM: the index is a signed list of files.
- Moving `game-unreal/Content` (menu, catalog tables, splash) anywhere: it
  is the client's, cooked into `Game/`.
- Splitting the pipeline out: `track-editor/` and `scripts/` stay with the
  code, since the server's loaders and the exporter share formats and move
  together ([../content/track-format.md](../content/track-format.md): the
  YAML is modelled twice already).
- Linux or macOS launchers; the server's `content` command line covers a
  headless host.

## Traps

- **Two repositories, one checksum.** A track YAML is modelled in the
  server and in `track-core`; a key added to one and not the other drops
  out of a pipeline rewrite, and the pack built by the content repository's
  CI then carries a different crc than the server computes. The content
  CI must build with the `apexsim` tag it declares in `requires`, not
  `main`.
- **A pack version is not the crc.** Two versions can share a crc (export
  rebuilt, YAML untouched); the server matches on crc, people on version.
  Never make the server compare versions.
- **Junctions.** `initialize_content.ps1 -Content` junctions folders into
  `content/`; a `git clean`, a worktree removal or an `rm -r` through a
  junction empties the content checkout. Remove the junction first, always.
- **The seed must stay sufficient.** Every server test, the determinism
  test, the client's automation tests and the showcases behind the menu
  must run on the seed alone; a test that reaches for Monza moves to the
  content repository's CI or to the seed.
- **`custom/` is still the player's.** A pack installs only into
  `default/`; an uploaded custom car on a server stays a dashboard upload,
  not a pack, and never ships.
