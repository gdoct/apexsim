# Promo video pipeline

Three steps turn a shot list into a finished promotional video. The race
director films a recorded race offline, and the game dumps every frame at a
fixed timestep.

```
scripts/promo/shots.yml ──make_clips.py──▶ out/promo/clips/*.mp4 ──stitch_video.py──▶ out/promo/ApexSim_promo.mp4
          │                    │
          │                    ├─ apexsim-replay simulate   headless AI race       -> out/promo/races/<race>.bin
          │                    ├─ apexsim-replay find       where the field is     -> the shot's window
          │                    ├─ apexsim-replay pose       where the camera stands
          │                    ├─ apexsim-replay cut        a few seconds of it    -> out/promo/cuts/<id>.clip.json
          │                    ├─ ApexSim -ApexReplay=...   plays + records        -> out/promo/frames/<id>/frame_*.png
          │                    └─ ffmpeg                    frames -> clip
```

## What you need on the render machine

| For | Needs |
|---|---|
| Planning (simulate, find, cut) | The Rust toolchain. Python 3 with `pyyaml`. |
| Rendering | The game built from this commit, editor build or packaged, with the tracks' levels imported. |
| Encoding and stitching | `ffmpeg` on PATH, or `--ffmpeg`. `pillow` for the cards and captions. |

```bash
pip install pyyaml pillow
python scripts/promo/make_clips.py        # every shot: plan, render, encode (about 1-3 min per shot)
python scripts/promo/stitch_video.py --music path/to/track.mp3
```

**Finding the game.** `make_clips.py` looks for it in this order:

1. `--game`
2. `$APEXSIM_GAME`
3. The newest `artifacts/release/*/Game/ApexSim.exe`
4. `artifacts/ApexSim-Win64/**/ApexSim.exe`
5. `UnrealEditor.exe` for the project's engine (`$UE_ROOT`, else the launcher's default install), run on the `.uproject` with `-game`

The editor build is the quickest to iterate on. The first launch compiles shaders.

## The stages

- **Plan** (`--stage plan`). This needs no game. Each race in `races:` is simulated once and cached in `out/promo/races/`, about 200 MB each. `--drop-races` deletes them after the cut. A race's `seed` fixes the grid, so the same spec replays the same race tick for tick and every planned moment stays put. Change the seed for a different race.
- **Render** (`--stage render`). The game starts on each clip with `-ApexReplayRecord=`, writes `frame_000000.png` onward and `replay_done.json`, and quits. Render time does not affect the result, because game time steps exactly one frame per frame.
- **Encode** (`--stage encode`). ffmpeg writes H.264 (CRF 14) by default, or ProRes 422 HQ with `--codec prores` for an editor. `--keep-frames` keeps the PNGs.
- **Preview** (`--preview <id>`). This plays one shot live and looping, with no recording, so you can tune its camera. The console's `apexsim.cam.*` and `apexsim.tv.*` commands work during it.
- **Dry run** (`--dry-run`). This plans, then prints the game and ffmpeg commands.

## shots.yml

**Races.** Each entry sets `track` (the YAML stem), `car`, `ai`, `laps`, `weather`, `time`, `seed`, `max_seconds` and `countdown`. The car is the host car, and the field is its class dealt round-robin, as the server deals it. Set `same_car: true` to put everyone in the host car.

`weather` changes the grip the race is simulated with. A shot's own `time` and `weather` only change the look. For example, `11_lemans_night` re-lights the dusk race at 23:40.

**Shots.** A shot names a `race`, a `window`, a `length` in race seconds, a `camera`, and optionally `caption`, `slowmo`, `time`, `weather` and `extra_args`.

**Windows.**

| `window` | The shot starts |
|---|---|
| `{corner: "Eau Rouge", before: 120, after: 180, min_cars: 2}` | Around the busiest moment in that stretch of the lap. `corner` matches a name in the track's layout dossier; `station` takes metres instead. |
| `start` | At the lights going out, plus `start_offset_s`. |
| `finish` | At the winner taking the flag, plus `start_offset_s`. |
| `{at_s: 123.4}` | At that many seconds into the recording. |

A corner window also takes these keys:

- `anchor` puts the busiest frame that far into the shot, 0 to 1.
- `pick` takes the n-th busiest moment.
- `last_laps: 1` looks only at the final lap.
- `include_lap_1` also counts the first lap, where the start queue runs.

**Cameras.**

| `mode` | What it does | Keys |
|---|---|---|
| `tv` | The broadcast director cuts as it likes. | `shot` holds one kind: grid, trackside, helicopter, tracking, chase, onboard, nose or reverse. `follow` locks it on one car. |
| `fixed` | A tripod. The cars drive through the frame. | `eye`, `look`, `fov` |
| `pan` | A tripod that turns after a car. | `eye`, `look`, `look_bias`, `fov`, `frame_width`, `pan_speed` |
| `chase` | The chase camera on one car. | `chase`: roof, close, near or far |
| `cockpit` | The driver's eye. | |

**Camera points.** An `eye` or `look` is `{corner|station, offset, lateral, side, height}`. `side` is left, right, outside or inside, where outside and inside are relative to the bend at that station. A point can instead be `{landmark: <name or kind>, height}`. Landmarks, crossings, stands and buildings in the dossier all count, such as `big_wheel`, `statue` or `Tyre bridge`.

The eye is seated on the baked ground heightfield when the track has one. The client also lifts a tripod that ends up under the rendered ground to 1.2 m above it (`ground_clearance`).

**Pan keys.**

- `look_bias` turns the camera that share of the way from the car toward the look point, so a landmark stays in frame.
- `frame_width` zooms to keep that many metres across the frame at the car. `fov` is then the widest the lens goes and `min_fov` the tightest.
- `pan_speed` sets how fast the camera turns after the car.

**Who is followed.**

| `follow` | The camera watches |
|---|---|
| `lead` | The car leading through the window. |
| `leader` | The race leader. |
| `winner` | The winner. |
| `nearest` | The car nearest the look point, re-picked as the field goes by. |
| A number | That car index. |

**Slow motion.** `slowmo: 0.5` renders at twice the frame rate and encodes at the normal one. Every frame is real, and the shot runs twice as long on screen.

**Edit.** This section sets the `sequence` in play order, with an optional per-shot `transition` (any ffmpeg `xfade` name) and `crossfade_s`. It also sets the `title` and `end` cards (text, subtitle, background art), `captions`, `grade` (contrast, saturation, vignette), `font` and `music`. `--draft` stitches at half resolution in seconds.

## Doing it by hand

```bash
cd server && cargo build --release --bin apexsim-replay && cd ..
R=server/target/release/apexsim-replay

$R simulate --track content/tracks/real/Zandvoort.yaml --car yotota-lmp2 --ai 12 --laps 3 \
   --weather sunny --time 19:10 --countdown 7 --seed 1 --out out/zandvoort.bin
$R info out/zandvoort.bin                  # laps, finish order, start and finish ticks
$R find out/zandvoort.bin --corner Luyendyk --before 300 --after 150 --min-cars 3 --last-laps 1
$R cut  out/zandvoort.bin --from-s 330 --to-s 345 --out out/luyendyk.clip.json
$R pose --track content/tracks/real/Zandvoort.yaml --corner Luyendyk --offset 40 \
   --lateral 28 --side outside --height 8 --look-offset -30 --look-height 1
```

```
UnrealEditor.exe game-unreal/ApexSim.uproject -game -ApexReplay=out/luyendyk.clip.json
    -ApexReplayCam=pan -ApexCameraLookAt=<from pose> -ApexReplayFollow=nearest -ApexReplayFrameWidth=20
    [-ApexReplayRecord=out/frames/luyendyk -ApexReplayFps=60 -ApexReplayRes=1920x1080 -nosound]
```

Every client switch is listed in `ApexReplaySubsystem.h`:

- `-ApexReplayStart=` and `-ApexReplayDuration=` set the span, in clip seconds.
- `-ApexReplayPreroll=` rolls unrecorded time before the start (default 1 s). `-ApexReplayWarmup=` holds on the first frame while the level settles (default 3 s).
- `-ApexReplaySeed=` sets the TV director's seed.
- `-ApexReplayTimeOfDay=` and `-ApexReplayWeather=` override the sky.
- `-ApexReplayLoop` loops playback. `-ApexReplayShowUi` keeps the menu visible. `-ApexReplayNoExit` keeps the game open after recording.

## Formats

- **Replay (`.bin`).** This is the server's own replay format (`replay.rs`), version 2. Its header adds the conditions, the start tick, the track stem and the lap length. A version 1 file still reads.
- **Clip (`.clip.json`).** This is `replay_tools::ClipFile`. Every frame lists every car in roster order as 16 numbers: position, yaw, pitch, roll, speed, throttle, brake, steering, gear, rpm, lap, station, on-track and finish position. The client (`FApexReplayClip`) blends between frames by game time: angles the short way round, and the station across the line. It never blends across a jump of more than 50 m.

## Known limits

- **No audio.** Audio runs in real time and the frame dump does not, so none is recorded. Put music over the cut.
- **Weather overrides are visual only.** A shot's weather changes the sky, not the grip. Simulate the race in the rain for rain driving.
- **Spa has no Eau Rouge dip.** Spa's centerline elevation is wrong through Eau Rouge and Raidillon: it crests where the real road dips and climbs. The shot works, but the dip will not look dramatic until the track data is fixed.
