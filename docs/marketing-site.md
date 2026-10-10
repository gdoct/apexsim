# Marketing page and promo video

Two offline pipelines that show the game to people who do not have it: the
marketing page (`docs/index.html`, served by GitHub Pages from `docs/` on
`main`) and the promotional video. Both film races nobody drove: seeded
headless AI races from `apexsim-replay`, played back in the game under the
replay cameras (see [spectator](game/spectator.md)).

## Code

- `site/`: the page's source (`template.html`, `site.css`, `site.js`,
  `copy.yml`, `media.yml`, `shots.yml`, `shots/`, `copy.lock.json`,
  `README.md`).
- `scripts/site/build_site.py` (the generator), `scripts/site/facts.py`
  (facts from content), `scripts/site/make_shots.py` (in-engine pictures),
  `scripts/site/tests/`.
- `scripts/promo/shots.yml`, `scripts/promo/make_clips.py`,
  `scripts/promo/stitch_video.py`.
- `.claude/commands/site-refresh.md`: the `/site-refresh` command.
- CI: the `site` job in `.github/workflows/ci.yml`.

## The page

`docs/index.html` and `docs/assets/` are **generated**: never edit them.

```powershell
python scripts/site/build_site.py              # -> docs/index.html, docs/assets
python scripts/site/build_site.py --check      # CI: exit 1 when docs/ is behind
python scripts/site/build_site.py --stale      # doc sections changed since the copy was reviewed
python scripts/site/build_site.py --reviewed   # record them as covered
python scripts/site/build_site.py --force      # encode every picture again
python scripts/site/make_shots.py [id ...]     # the in-engine pictures (opens the game)
python -m unittest discover -s scripts/site/tests
```

Three kinds of content, refreshed three ways:

- **Facts** (`facts.py`), read on every build: the car cards from every
  `content/cars/default/*/car.toml`, the circuits from the track YAMLs
  (display name, metadata, an SVG outline of the centerline, the dossier's
  corner display names) and the counts the copy quotes (`n.cars`,
  `n.tracks`, `n.km`, `n.setup_knobs` from the server's `KNOB_COUNT` in
  `car_setup.rs`, `n.hud_components`...). A change to a car.toml, a track
  YAML or a car render therefore fails `--check` until `docs/` is rebuilt and
  committed. Display names only: `facts.display_class` mirrors
  `ApexCatalog::DisplayClass` (see [naming](content/naming.md)).
- **Words** (`site/copy.yml`): hand-written, every string a Jinja template
  over the facts (`{{ n.cars|words }}`, `{{ name('Spa') }}`,
  `{{ car('posh-gt3rs') }}`), so no number is typed.
- **Media** (`site/media.yml`): each picture's source file in the repo and how
  to cut it (`width`, `aspect`, `focus`), encoded to WebP.
  `docs/assets/manifest.json` records the hash of each picture's source, which
  is what `--check` compares (two WebP encoders need not agree on bytes). An
  entry's `zoom: px` adds `<name>-zoom.webp`, the uncropped frame, which a
  click opens in a `<dialog>` (`site.js`: any `img[data-zoom]`). Car pictures
  are the Blender previews, `content/props/_preview/cars/<folder>_hero.png`
  (`scripts/content/cars/preview_cars.py`). The template names a picture only
  through `asset()`, which fails the build on one `media.yml` lacks.

### Keeping the words current

The build cannot tell when prose is stale, so it watches the docs instead.
`FEATURE_DOCS` in `build_site.py` is every `.md` under `docs/` except
`docs/proposals/` and `README.md` files. `--reviewed` hashes each section
(heading to next heading) of those docs into `site/copy.lock.json`; `--stale`
lists the sections new, changed or removed since. In CI that is a warning, not
a failure. The `/site-refresh` command is the rewrite pass: read the stale
sections, update `copy.yml` against the code, [simulation gaps](simulation-gaps.md)
and the [roadmap](roadmap.md) (promise nothing deferred), rebuild, look at the
page, then `--reviewed`, `--check` and the unit tests. It does not commit.

### In-engine pictures (`site/shots.yml` -> `site/shots/<id>.webp`)

The picked masters are checked in. Two kinds:

- **action**: the promo pipeline (`make_clips.Planner` and `render`): a seeded
  AI race, the moment found by `window`, played with `-ApexReplay` under the
  TV director or a tripod, no HUD. `frames` candidates go to
  `out/site/candidates/<id>/` with a `sheet.jpg`; the shot's `pick` names the
  one used. The races replay bit for bit, so a retake after a visual change is
  the same moment and the pick holds (`make_shots.py --pick` re-copies picks).
- **ui**: live runs (`-ApexAutoRace`, `-ApexTrack=`, `-ApexOpenSettings=`...,
  `-ApexScreenshotAfter`), with `r.SetRes` because settings.yml's borderless
  mode otherwise makes the grab the monitor's size, and
  `DisableAllScreenMessages`; the game is killed once the grabs are on disk
  (it does not quit by itself). A retake is a new picture: look at the sheet
  again.

## The promo video

```
scripts/promo/shots.yml --make_clips.py--> out/promo/clips/*.mp4 --stitch_video.py--> out/promo/ApexSim_promo.mp4
   apexsim-replay simulate   headless AI race    -> out/promo/races/<race>.bin
   apexsim-replay find       where the field is  -> the shot's window
   apexsim-replay pose       where the camera stands
   apexsim-replay cut        a few seconds       -> out/promo/cuts/<id>.apxs
   ApexSim -ApexReplay=...   plays and records   -> out/promo/frames/<id>/frame_*.png
   ffmpeg                    frames -> clip
```

```bash
pip install pyyaml pillow                      # plus ffmpeg on PATH (or --ffmpeg)
python scripts/promo/make_clips.py             # every shot: plan, render, encode
python scripts/promo/stitch_video.py --music path/to/track.mp3 [--draft]
```

**The game** is found by `--game`, `$APEXSIM_GAME`, the newest
`artifacts/release/*/Game/ApexSim.exe`, `artifacts/ApexSim-Win64/**/ApexSim.exe`,
then the editor (`UnrealEditor.exe` for the project's engine, `$UE_ROOT` or
the launcher's default install, with `-game`). The editor build is quickest to
iterate on.

**Stages** (`--stage plan|render|encode|all`):

- *plan* needs no game: each race in `races:` is simulated once and cached in
  `out/promo/races/` (about 200 MB each; `--drop-races` deletes them after the
  cut, `--resimulate` forces a rerun). A race's `seed` fixes the grid, so every
  planned moment stays put.
- *render* starts the game per clip with `-ApexReplayRecord=`; it writes
  `frame_000000.png` onward and `replay_done.json`, then quits. Game time steps
  exactly one frame per frame, so render speed does not affect the result.
- *encode*: H.264 (CRF 14) by default, `--codec prores` for an editor;
  `--keep-frames` keeps the PNGs.
- `--preview <id>` plays one shot live and looping (no recording) to tune its
  camera; `apexsim.cam.*` and `apexsim.tv.*` work meanwhile. `--dry-run`
  plans and prints the commands.

### shots.yml

- **Races**: `track` (stem), `car` (the host; the field is its class dealt
  round-robin, `same_car: true` puts everyone in it), `ai`, `laps`, `weather`,
  `time`, `seed`, `max_seconds`, `countdown`, `cars_dir`
  (`content/cars/default` keeps the player's own cars out). A race's `weather`
  sets the grip it is simulated with; a shot's own `time` and `weather` change
  only the look.
- **Shots**: `race`, `window`, `length` (race seconds), `camera`, and
  optionally `caption`, `slowmo`, `time`, `weather`, `extra_args`.
- **Windows**: `{corner: "Eau Rouge", before, after, min_cars}` (the busiest
  moment in that stretch; `station` instead of `corner`; `anchor`, `pick`,
  `last_laps`, `include_lap_1`), `start` or `finish` (plus `start_offset_s`),
  or `{at_s: 123.4}`.
- **Cameras** (`mode`): `tv` (the broadcast director; `shot` holds one kind,
  `follow` locks a car), `fixed` (`eye`, `look`, `fov`), `pan` (adds
  `look_bias`, `frame_width`, `min_fov`, `pan_speed`), `chase`
  (`chase: roof|close|near|far`), `cockpit`. An `eye` or `look` is
  `{corner|station, offset, lateral, side, height}` (`side` left, right,
  outside or inside of the bend) or `{landmark: <name or kind>, height}`. The
  eye is seated on the ground heightfield; the client lifts a tripod under the
  rendered ground clear of it (`ground_clearance`).
- **follow**: `lead` (leading through the window), `leader`, `winner`,
  `nearest` (re-picked as the field goes by), or a car index.
- **slowmo**: `0.5` renders at twice the frame rate and encodes at the
  normal one: every frame is real.
- **Edit**: the `sequence` in play order with per-shot `transition` (an ffmpeg
  `xfade` name) and `crossfade_s`, `title` and `end` cards, `captions`,
  `grade`, `font`, `music`.

By hand, the same steps are the `apexsim-replay` commands in
[spectator](game/spectator.md) followed by
`UnrealEditor.exe game-unreal/ApexSim.uproject -game -ApexReplay=<clip>.apxs
-ApexReplayCam=pan -ApexCameraLookAt=<from pose> -ApexReplayFollow=nearest
[-ApexReplayRecord=<dir> -ApexReplayFps=60 -ApexReplayRes=1920x1080]`. Every
client switch (`-ApexReplayStart`, `-ApexReplayDuration`,
`-ApexReplayPreroll`, `-ApexReplayWarmup`, `-ApexReplaySeed`,
`-ApexReplayTimeOfDay`, `-ApexReplayWeather`, `-ApexReplayLoop`,
`-ApexReplayShowUi`, `-ApexReplayNoExit`, the camera switches) is listed in
the header of `ApexReplaySubsystem.h`.

## Checking it

- `python scripts/site/build_site.py --check` and the unit tests (CI runs
  both).
- `python scripts/promo/make_clips.py --dry-run`, then `--preview <id>`.
- Server side: `replay_tools::tests` (windows, cut, poses, a seeded race).

## Traps

- Keep the player's own cars off the page and the video: races are dealt
  from `content/cars/default` (`cars_dir`), and the ui shots' server is
  started with `APEXSIM_CONTENT_CARS_DIR` pointing there, because an AC import
  carries a real team's colours.
- A fixed tripod (`mode: pan` with an `eye`) can end up inside a stand after a
  track is re-dressed; the TV director's `trackside` shot traces for
  visibility and does not.
- A ui run rewrites the profile's "continue where you left off" car, track
  and mode.
- No audio is recorded (audio runs in real time, the frame dump does not):
  put music over the cut.
- A shot's weather override is visual only; simulate the race in the rain for
  rain driving.
