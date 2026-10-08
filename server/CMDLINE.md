# Command line options for apexsim-server

This document lists the command line options of the `apexsim-server`
executable and the environment variables it reads at startup. Most of the
server's behaviour is set in `server.toml`; the command line only says where
that file is and how much to log, and the environment variables override
single settings from it.

## Usage

```
apexsim-server [OPTIONS]
```

From a source checkout, pass the options after `--`:

```bash
cd server
cargo run --release -- --config server.toml --log-level debug
```

In a release package the server is `Server\apexsim-server.exe`. Run it from
that folder (double-clicking it does): `content/`, `showcase/`, `records/`
and the TLS paths in `server.toml` are resolved relative to the working
directory.

## Options

| Option | Default | Description |
|---|---|---|
| `-c`, `--config <PATH>` | `./server.toml` | The configuration file. If it does not exist the server starts on the built-in defaults and says so on stderr and in the log. If it exists but does not parse, or fails validation, the server refuses to start. |
| `-l`, `--log-level <LEVEL>` | `[logging] level` from the config (`info`) | Log level: `trace`, `debug`, `info`, `warn` or `error`. Also accepts a `tracing` filter such as `apexsim_server=debug,warn`. |
| `--generate-terrain` | off | Generate procedural terrain for every track under `[content] tracks_dir` whose metadata has an `environment_type`, write it to the cache files, and exit without starting the server. |
| `-h`, `--help` | | Print the options and exit. |
| `-V`, `--version` | | Print the version and exit. |

### Which log level wins

1. `RUST_LOG`, when it is set and valid, overrides everything else.
2. Otherwise `--log-level`.
3. Otherwise `APEXSIM_LOGGING_LEVEL`, then `[logging] level` in the config.

## Environment variables

Each `APEXSIM_*` variable below overrides one setting of `server.toml`. They
are applied after the file is loaded (or after the defaults when there is no
file), and each override is logged at startup. A value that does not parse
(a port that is not a number, a boolean that is not `true`/`false`) is logged
as an error and ignored; the setting keeps its value from the file.

The `_PORT` variables replace only the port of the matching bind address and
keep its host, so `APEXSIM_NETWORK_TCP_PORT=9100` turns `127.0.0.1:9000` into
`127.0.0.1:9100`. When both a `_BIND` and a `_PORT` variable are set, the
port is applied after the bind.

### Server

| Variable | Setting | Default |
|---|---|---|
| `APEXSIM_SERVER_TICK_RATE_HZ` | `[server] tick_rate_hz` (30-1000) | `420` |
| `APEXSIM_SERVER_MAX_SESSIONS` | `[server] max_sessions` | `8` |

### Network

| Variable | Setting | Default |
|---|---|---|
| `APEXSIM_NETWORK_TCP_BIND` | `[network] tcp_bind`: login, lobby, sessions | `127.0.0.1:9000` |
| `APEXSIM_NETWORK_TCP_PORT` | port of `tcp_bind` | |
| `APEXSIM_NETWORK_UDP_BIND` | `[network] udp_bind`: telemetry and input | `127.0.0.1:9001` |
| `APEXSIM_NETWORK_UDP_PORT` | port of `udp_bind` | |
| `APEXSIM_NETWORK_HEALTH_BIND` | `[network] health_bind`: `/health`, `/ready`, `/metrics` | `127.0.0.1:9002` |
| `APEXSIM_NETWORK_HEALTH_PORT` | port of `health_bind` | |
| `APEXSIM_NETWORK_TLS_CERT_PATH` | `[network] tls_cert_path` | |
| `APEXSIM_NETWORK_TLS_KEY_PATH` | `[network] tls_key_path` | |
| `APEXSIM_NETWORK_REQUIRE_TLS` | `[network] require_tls` (`true`/`false`) | `true` |
| `APEXSIM_NETWORK_TELEMETRY_DIVISOR` | `[network] telemetry_divisor`: telemetry goes out every Nth tick | `7` (60 Hz at 420 Hz) |

The default binds are loopback only. To host for other machines bind to
`0.0.0.0` (e.g. `APEXSIM_NETWORK_TCP_BIND=0.0.0.0:9000` and the same for
UDP) and open 9000/tcp and 9001/udp.

### Content

| Variable | Setting | Default |
|---|---|---|
| `APEXSIM_CONTENT_CARS_DIR` | `[content] cars_dir` | `../content/cars` |
| `APEXSIM_CONTENT_TRACKS_DIR` | `[content] tracks_dir` | `../content/tracks` |
| `APEXSIM_SHOWCASE_ENABLED` | `[showcase] enabled` (`true`/`false`) | `true` |
| `APEXSIM_SHOWCASE_DIR` | `[showcase] dir`: the `.apxs` races streamed to menus | `./showcase` |

The content folders are read as `default/` then `custom/` when they have
those subfolders. Pointing `APEXSIM_CONTENT_CARS_DIR` at
`content/cars/default` leaves the player's own cars out.

### Logging and physics

| Variable | Setting | Default |
|---|---|---|
| `APEXSIM_LOGGING_LEVEL` | `[logging] level` | `info` |
| `APEXSIM_PHYSICS_ROAD_CONTACT` | `[physics] road_contact`: `mesh` drives on a track's baked road mesh when it has one, `centerline` on the centerline alone | `mesh` |
| `RUST_LOG` | log filter, wins over every other log setting | |

### Admin dashboard

A web page for operators, on the same machine by default
(`docs/server/ADMIN_DASHBOARD.md` in the source repository).

| Variable | Setting | Default |
|---|---|---|
| `APEXSIM_ADMIN_ENABLED` | `[admin] enabled` (`true`/`false`) | `true` |
| `APEXSIM_ADMIN_HTTP_BIND` | `[admin] http_bind`; empty switches HTTP off | `127.0.0.1:9003` |
| `APEXSIM_ADMIN_HTTP_PORT` | port of `http_bind` | |
| `APEXSIM_ADMIN_HTTPS_BIND` | `[admin] https_bind`; empty switches HTTPS off | `127.0.0.1:9004` |
| `APEXSIM_ADMIN_HTTPS_PORT` | port of `https_bind` | |
| `APEXSIM_ADMIN_TOKEN` | `[admin] token`; empty makes a random one, printed once in the log | |

### Debug

Never on a server people race on.

| Variable | Setting | Default |
|---|---|---|
| `APEXSIM_DEBUG_STAND_IN_DRIVER` | `[debug] stand_in_driver` (`true`/`false`): an AI drives every human's car | `false` |
| `APEXSIM_DEBUG_EVENTS` | `[debug] events`: timed events on cars, `"<s of green>:<host\|all\|car index>:<action>;..."` (actions: `wear=P`, `puncture=FL`, `leak=FL:KPA`, `flatspot=FL:S`, `damage=<zone>:P`, `brakewear=P`, `pit`, `boost=S`, `aids=off`) | empty |

Settings with no environment variable (`[auth]`, `[ai]`, `[records]`, the
rest of `[showcase]` and `[admin]`, ...) are only set in `server.toml`.

## Examples

```bash
# Default config in the working directory
apexsim-server

# Another config file, verbose logging
apexsim-server --config /etc/apexsim/server.toml --log-level debug

# Host on every interface, without TLS (a LAN game)
APEXSIM_NETWORK_TCP_BIND=0.0.0.0:9000 \
APEXSIM_NETWORK_UDP_BIND=0.0.0.0:9001 \
APEXSIM_NETWORK_REQUIRE_TLS=false \
apexsim-server

# A second server beside the first, on other ports
APEXSIM_NETWORK_TCP_PORT=9100 APEXSIM_NETWORK_UDP_PORT=9101 \
APEXSIM_NETWORK_HEALTH_PORT=9102 APEXSIM_ADMIN_ENABLED=false \
apexsim-server
```

On Windows (PowerShell) set the variables first:

```powershell
$env:APEXSIM_NETWORK_TCP_BIND = "0.0.0.0:9000"
$env:APEXSIM_NETWORK_UDP_BIND = "0.0.0.0:9001"
.\apexsim-server.exe --log-level debug
```

## Stopping

Ctrl+C (SIGINT), or SIGTERM on Linux (systemd, Docker, Kubernetes), shuts
the server down cleanly: running sessions are closed and buffered log lines
flushed.

## Other executables

The server crate builds two more tools. `apexsim-replay` ships in a release
package beside the server (`Server\apexsim-replay.exe`); `convert_track` is
only in a source build.

```bash
cd server
cargo build --release          # all three: target/release/apexsim-server, apexsim-replay, convert_track
cargo run --release --bin apexsim-replay -- render --help
cargo run --release --bin convert_track -- --help
```

---

# apexsim-replay

Runs AI races offline on the server's own simulation (no network, no
client), and cuts, inspects and converts the recordings. It is what makes
the menu's backdrop races (showcases), the promo and website clips and the
track guides.

```
apexsim-replay <COMMAND> [OPTIONS]
```

| Command | What it does |
|---|---|
| [`simulate`](#simulate) | Run a headless AI race and write it as a replay (`.bin`). |
| [`render`](#render) | Run a headless AI race and write it as a spectator stream (`.apxs`). |
| [`convert`](#convert) | Turn a replay (`.bin`) into a spectator stream (`.apxs`). |
| [`info`](#info) | Print what a replay or a stream holds; check a stream against the content. |
| [`find`](#find) | Find the moments the field runs through a stretch of the lap together. |
| [`cut`](#cut) | Cut a replay down to a window, as `.apxs`, `.bin` or `.json`. |
| [`guide`](#guide) | Build the track guides (corner-by-corner walks) per track and class. |
| [`pose`](#pose) | Work out a camera position beside the road for a screenshot or clip. |

### Conventions

- **Output.** Every command prints its result as pretty JSON on stdout, so
  a script can read it; progress and warnings go to stderr.
- **Exit code.** 0 on success; 1 on any error, with
  `apexsim-replay: <message>` on stderr.
- **Paths.** The defaults (`content/cars`, `content/tracks`, `build/guide`)
  are relative to the working directory: run from the repository root, or
  from `Server\` in a release package, which holds `content\cars` and
  `content\tracks`. A command that is given a replay but no `--track` finds
  the YAML by the stem recorded in the replay, under
  `content/tracks/default` then `content/tracks/custom`.
- **Cars.** A car is named by its folder, its `id` or its name. The field
  races in the host car's class: every car of that class, dealt in turn.
  `--cars-dir content/cars/default` leaves the player's own (custom and
  imported) cars out.
- **Weather** is one of `sunny` (`clear`), `cloudy`, `overcast`,
  `lightrain` (`rain`), `heavyrain` (`storm`), or `0`-`4`. Case, spaces and
  dashes are ignored. **Time** is local time as `hh:mm`.
- **Ticks.** The simulation runs at `--tick-rate` (default 420 Hz, the
  server's), so tick 4200 is ten seconds in. `find` reports windows in
  ticks, which `cut` and `render` take.
- **Determinism.** A race with a fixed seed is bit-for-bit the same every
  run (the same grid, the same AI, the same file).

## simulate

Runs a headless AI race and writes it as a replay (`.bin`, the same format
the live server records). Prints a summary of the race.

```
apexsim-replay simulate --track <YAML> --out <FILE> [OPTIONS]
```

| Option | Default | Description |
|---|---|---|
| `--track <YAML>` | required | The track YAML. |
| `--out <FILE>` | required | The replay to write. |
| `--car <CAR>` | `yotota-lmp2` | The host car; the field is its class. |
| `--same-car` | off | Every AI drives the host car instead of the class's mix. |
| `--cars-dir <DIR>` | `content/cars` | The cars folder. |
| `--ai <N>` | `12` | Number of AI cars. |
| `--laps <N>` | `3` | Race length in laps. |
| `--max-seconds <S>` | `900` | Stop after this much racing even if nobody has finished. |
| `--countdown <S>` | `3` | Seconds of countdown before green. |
| `--weather <W>` | `sunny` | The weather (see Conventions). |
| `--time <hh:mm>` | `13:00` | Time of day. |
| `--tick-rate <HZ>` | `420` | Simulation rate. |
| `--record-hz <HZ>` | `60` | Frames recorded per second of race. |
| `--seed <N>` | random | Fixes the grid (the AI drivers' ids): the same seed, the same race. |

```bash
apexsim-replay simulate --track content/tracks/default/Zandvoort.yaml --car yotota-lmp2 \
    --ai 12 --laps 3 --weather sunny --time 18:30 --seed 4 --out out/zandvoort.bin
```

## render

Runs a headless AI race and writes it as a spectator stream (`.apxs`):
the grid, the countdown, the laps and a tail past the winner's flag. This is
the file the menu plays behind its screens, the server streams as a
showcase, and the client plays with `-ApexReplay=<file>`. With `--seeds N`
it renders N races and keeps the best one: the least contact, off-road time
and retirements, and the closest racing.

Prints the output path, the seed kept, its score, the race statistics and a
description of the stream.

```
apexsim-replay render --track <YAML> (--class <CLASS> | --car <CAR>) --out <FILE> [OPTIONS]
```

| Option | Default | Description |
|---|---|---|
| `--track <YAML>` | required | The track YAML. |
| `--out <FILE>` | required | The stream to write (`.apxs`). |
| `--class <CLASS>` | | Car class (`GT3`, `LMP2`, `F1`, ...): the field is every car of it. One of `--class` and `--car` is required. |
| `--car <CAR>` | | Host car instead of `--class`; the field is its class. Wins over `--class`. |
| `--same-car` | off | Every car on the grid is the host car. |
| `--cars-dir <DIR>` | `content/cars` | The cars folder (`content/cars/default` keeps the player's own cars out). |
| `--cars <N>` | `20` | Grid size. |
| `--laps <N>` | `2` | Laps from green. |
| `--max-seconds <S>` | `1800` | Stop after this much racing even without a winner. |
| `--countdown <S>` | `8` | Seconds the grid stands before the lights go out. |
| `--tail <S>` | `10` | Seconds kept after the winner takes the flag. |
| `--weather <W>` | `sunny` | The weather. |
| `--time <hh:mm>` | `13:00` | Time of day. |
| `--air <C>` | from weather and time | Air temperature, °C (-5 to 45; negative values are fine). |
| `--wind <KPH>` | from the weather | Mean wind speed, km/h (0-60). |
| `--wind-from <DEG>` | from the seed | Where the wind blows from, degrees from the start straight (0 head-on, 90 from its left). |
| `--tick-rate <HZ>` | `420` | Simulation rate. |
| `--rate <FPS>` | `30` | Frames per second of the stream. |
| `--seed <N>` | `0` | The grid's seed: the same seed, the same file. |
| `--seeds <N>` | `1` | Render this many seeds, from `--seed` up, and keep the best. |
| `--pick <RULE>` | `best` | How the kept seed is chosen; `best` is the only rule. |
| `--from-tick <T>` / `--to-tick <T>` | whole race | Keep only a window of the race. |

```bash
apexsim-replay render --track content/tracks/default/Spa.yaml --class GT3 --cars 20 --laps 2 \
    --weather sunny --time 13:00 --seed 7 --seeds 4 --cars-dir content/cars/default \
    --out build/showcase/Spa.gt3.day.apxs
```

`content/showcase.yml` lists the showcases the content scripts render; a
circuit takes about six seconds and the file is a few MB.

## convert

Turns a replay (`.bin`) into a spectator stream (`.apxs`). A converted
stream has no pit, damage or hybrid data and no lap timing (a replay does
not record them). The track's checksum, centerline and sectors and the
cars' checksums are filled in when the content is at hand; without them the
stream still plays, and `info --check` has less to compare.

```
apexsim-replay convert <REPLAY> --out <FILE> [OPTIONS]
```

| Option | Default | Description |
|---|---|---|
| `<REPLAY>` | required | The replay to read. |
| `--out <FILE>` | required | The stream to write. |
| `--rate <FPS>` | `30` | Frames per second of the stream. |
| `--track <YAML>` | from the replay's stem | Track YAML for the checksum, path and sectors. |
| `--cars-dir <DIR>` | `content/cars` | For the cars' checksums. |
| `--from-tick <T>` / `--to-tick <T>` | whole replay | Convert only a window. |

```bash
apexsim-replay convert out/zandvoort.bin --rate 30 --out out/zandvoort.apxs
```

## info

Prints what a replay (`.bin`) or a stream (`.apxs`) holds as JSON: track,
conditions, cars, duration and so on. Both the server's replays and the
client's saved replays (`Saved/Replays/*.apxs`) can be read.

With `--check` (streams only) it also compares the track and every car the
stream was raced on against the content on disk, by checksum, prints what
no longer matches under `stale`, and exits with 1 if anything does. That
is how the content scripts decide which showcases to render again.

```
apexsim-replay info <FILE> [--check] [OPTIONS]
```

| Option | Default | Description |
|---|---|---|
| `<FILE>` | required | A replay or a stream. |
| `--check` | off | Exit non-zero when the track YAML or a car.toml changed since the stream was made. |
| `--tracks-dir <DIR>` | `content/tracks` | The tracks folder `--check` compares against. |
| `--cars-dir <DIR>` | `content/cars` | The cars folder `--check` compares against. |

```bash
apexsim-replay info build/showcase/Spa.gt3.day.apxs --check
```

## find

Finds the moments in a replay where several cars run through one stretch
of the lap together: the battles worth filming. The stretch is a corner
from the track's layout dossier or a station on the lap, plus a distance
before and after it. Prints the station, the stretch and the best windows,
each with its ticks, ready for `cut`.

```
apexsim-replay find <REPLAY> (--corner <NAME> | --station <M>) [OPTIONS]
```

| Option | Default | Description |
|---|---|---|
| `<REPLAY>` | required | A replay (`.bin`). |
| `--track <YAML>` | from the replay's stem | The track YAML. |
| `--corner <NAME>` | | A corner named in the track's layout dossier (`<Stem>.layout.json`); a part of the name is enough. Uses the corner's real `name`. |
| `--station <M>` | | A distance along the lap, metres, instead of a corner. |
| `--before <M>` | `150` | Metres before the station that count. |
| `--after <M>` | `100` | Metres after the station that count. |
| `--min-cars <N>` | `2` | Cars that must be in the stretch at once. |
| `--pad <S>` | `1.5` | Seconds added on either side of each window. |
| `--include-lap-1` | off | Count the first lap too (the queue after the start). |
| `--last-laps <N>` | all | Only while the leader is on its last N laps (the finish). |
| `--top <N>` | `5` | How many windows to print. |

```bash
apexsim-replay find out/zandvoort.bin --corner Hugenholtz --before 150 --after 120 --min-cars 3
```

## cut

Cuts a replay down to a window. The output's extension picks the format:

- `.apxs`: a spectator stream, which the client plays with
  `-ApexReplay=<file>`;
- `.json`: the older client clip format (needs the track YAML for the
  centerline);
- anything else (`.bin`): a replay.

Prints the output path, the frame count, the first and last tick and the
duration.

```
apexsim-replay cut <REPLAY> --out <FILE> [OPTIONS]
```

| Option | Default | Description |
|---|---|---|
| `<REPLAY>` | required | A replay (`.bin`). |
| `--out <FILE>` | required | The output; its extension picks the format. |
| `--from-tick <T>` / `--to-tick <T>` | start / end of the recording | The window, in simulation ticks. |
| `--from-s <S>` / `--to-s <S>` | | The window in seconds from tick 0, instead of ticks (ticks win if both are given). |
| `--track <YAML>` | from the replay's stem | Track YAML (the clip's centerline, the stream's checksum). |
| `--rate <FPS>` | `60` | Frames per second of an `.apxs` (at most the replay's own rate). |
| `--cars-dir <DIR>` | `content/cars` | For an `.apxs`'s car checksums. |

It fails on an empty window, or one with no frames in it (the message says
which ticks the recording covers).

```bash
apexsim-replay cut out/zandvoort.bin --from-tick 24000 --to-tick 26400 --out out/hugenholtz.apxs
```

## guide

Builds the track guides the track picker opens: for each track and car
class, `<Stem>.<Class>.guide.json` (the corners, what the car does in each,
the hints and the camera positions) and `<Stem>.<Class>.guide.apxs` (the
recording it plays: a few solo laps by cars of the class, merged a couple
of seconds apart). Notes from `<Stem>.guide.yml` beside the YAML are
attached to the nearest corner.

Prints the files written and the failures, and exits with 1 if any guide
failed. Progress, one line per guide, goes to stderr.

```
apexsim-replay guide (<YAML>... | --all) [OPTIONS]
```

| Option | Default | Description |
|---|---|---|
| `<YAML>...` | | One or more track YAMLs. |
| `--all` | off | Every track YAML in `<tracks-dir>/default` (the shipped circuits). |
| `--tracks-dir <DIR>` | `content/tracks` | Where `--all` looks. |
| `--class <LIST>` | `all` | Car classes, comma separated, or `all` (every class under `--cars-dir`). |
| `--cars-dir <DIR>` | `content/cars/default` | The cars folder. |
| `--out <DIR>` | `build/guide` | Where the guides go. |
| `--report` | off | Print each track's detected corners (number, name, direction, entry/apex/exit station, turn, tightest radius) and simulate nothing. Use it to write notes against. |
| `--missing-only` | off | Skip a guide whose file is newer than the track YAML, its notes, its dossier and every car.toml of the class. |
| `--seed <N>` | `1` | First seed. |
| `--tries <N>` | `6` | Attempts per car (at a lower skill each time) before the least bad lap is kept. |
| `--skill <N>` | `110` | The AI's skill for the guide laps (70-110). |
| `--gap <S>` | `2` | Seconds between the cars in the recording. |
| `--cars <N>` | `3` | Cars per guide. |
| `--rate <FPS>` | `60` | Frames per second of the recording. |

```bash
apexsim-replay guide --all                          # every shipped circuit x every class (a few minutes)
apexsim-replay guide content/tracks/default/Spa.yaml --class GT3
apexsim-replay guide --all --report                 # detected corners only
apexsim-replay guide --all --missing-only           # only what is out of date
```

## pose

Works out where to put a camera: an eye beside the road at a corner or
station and a point to look at, seated on the ground heightfield when the
track has one. Prints the eye and target in the server's frame (metres, +X
along the start straight, +Y left), the yaw and pitch, and the same pose
as the client's command-line switches `-ApexCamera=X,Y,Z,Yaw,Pitch` and
`-ApexCameraLookAt=X,Y,Z,TX,TY,TZ`.

```
apexsim-replay pose --track <YAML> (--corner <NAME> | --station <M>) [OPTIONS]
```

| Option | Default | Description |
|---|---|---|
| `--track <YAML>` | required | The track YAML. |
| `--corner <NAME>` | | A corner from the layout dossier (a part of the name is enough). |
| `--station <M>` | | A distance along the lap, metres, instead of a corner. |
| `--offset <M>` | `0` | The eye's distance along the lap from the corner (negative: before it). |
| `--lateral <M>` | `20` | The eye's distance from the centerline: positive left, negative right, or on `--side`. |
| `--side <SIDE>` | | `left`, `right`, `outside` or `inside` (of the bend at the eye), instead of the sign of `--lateral`. |
| `--height <M>` | `4` | The eye's height above the ground. |
| `--look-offset <M>` | `0` | Where the camera looks, metres along the lap from the corner. |
| `--look-lateral <M>` | `0` | The look point's distance from the centerline. |
| `--look-side <SIDE>` | | Side for `--look-lateral`, as `--side`. |
| `--look-height <M>` | `1` | The look point's height. |
| `--look-landmark <NAME>` | | Look at a dossier landmark, crossing, stand or structure by name or kind (`big_wheel`, `Tyre bridge`) instead of a point on the road; raised by `--look-height`. |

```bash
apexsim-replay pose --track content/tracks/default/Spa.yaml --corner "Eau Rouge" \
    --offset -60 --side outside --lateral 25 --height 6
```

---

# convert_track

Converts a circuit from the
[racetrack-database](https://github.com/TUMFTM/racetrack-database) CSV
format into an ApexSim track file (YAML or JSON). It reads a centerline
with the track width either side, and optionally a racing line, and writes
nodes, raceline and metadata. Only in a source build:

```
cargo run --release --bin convert_track -- --tracks-csv <CSV> --output <FILE> --name <NAME> [OPTIONS]
```

| Option | Default | Description |
|---|---|---|
| `-t`, `--tracks-csv <CSV>` | required | The centerline: `x_m,y_m,w_tr_right_m,w_tr_left_m` per line. |
| `-r`, `--raceline-csv <CSV>` | none | The racing line: `x_m,y_m` per line. |
| `-o`, `--output <FILE>` | required | The track file to write. |
| `-n`, `--name <NAME>` | required | The track's real name (`name`; kept in the data, never shown). |
| `--display-name <NAME>` | none | The name the game shows (`display_name`). Real circuit names are mostly trademarks: give a sound-alike here. Without it the game shows `name`. |
| `--country <TEXT>` | none | `metadata.country`. |
| `--city <TEXT>` | none | `metadata.city`. |
| `--category <TEXT>` | none | `metadata.category` (`F1`, `DTM`, `IndyCar`, ...). |
| `--year-built <YEAR>` | none | `metadata.year_built`. |
| `--description <TEXT>` | none | `metadata.description` (shown in the track picker). |
| `-f`, `--format <FMT>` | from the extension | `yaml` (or `yml`) or `json`. An unknown extension with no `--format` is an error. |
| `--friction <F>` | `1.0` | The friction of every node. |
| `--closed-loop <BOOL>` | `true` | `--closed-loop false` writes a point-to-point track (`closed_loop: false`, and the length has no closing segment). |
| `--track-id <UUID>` | kept, else new | The track's fixed id. Without it, converting over an existing file keeps that file's `track_id`, and only a new file gets a fresh random one. |

What it reads and writes:

- **CSV files.** The first line is a header and is always skipped, as are
  empty lines and lines starting with `#`. A line with too few columns is
  skipped with a warning; a value that is not a number stops the
  conversion. Coordinates and widths are metres.
- **Nodes** take the CSV's points as they are (`width_left`,
  `width_right`), on asphalt, with no banking and `z = 0` (the source data
  is 2D). `default_width` is the mean total width. `metadata.length_m` is
  measured from the centerline, with the closing segment on a closed loop.
- **Not generated:** checkpoints, sector lines (the lap is then split into
  thirds), grid slots, DRS zones and the location metadata (altitude,
  latitude, longitude).
- **`track_id` stays fixed.** The client's catalog rows and the lap
  records are keyed by it, so converting again over the same output file
  keeps its id. Converting to a new file mints one; pass `--track-id` to
  carry an id over, or to give the track a known one.

The result is a raw centerline. To turn it into a finished circuit, put the
YAML in `content/tracks/custom/` and run it through the track pipeline
(smoothing, banking, scene, export; see the track sections of `CLAUDE.md`
in the source repository).

```bash
cargo run --release --bin convert_track -- \
    --tracks-csv racetrack-database/tracks/Monza.csv \
    --raceline-csv racetrack-database/racelines/Monza.csv \
    --output content/tracks/custom/MyMonza.yaml \
    --name "Autodromo Nazionale Monza" --display-name "Monzza" \
    --country Italy --city Monza --category F1
```
