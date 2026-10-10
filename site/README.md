# The marketing page's source

`docs/index.html` (published by GitHub Pages from `main` / `docs`) is
generated from this folder. Nothing under `docs/index.html` or
`docs/assets/` is edited by hand.

| File | What it holds | Changes when |
|---|---|---|
| `template.html`, `site.css`, `site.js` | the page's structure, look and behaviour | the design changes |
| `copy.yml` | every word: hero, feature cards, class blurbs, captions | the game gains a feature worth telling |
| `media.yml` | which picture goes where, cut from which file in the repo | a picture is added or swapped |
| `shots.yml` | the in-engine shot list | a shot is added, or its camera or `pick` changes |
| `shots/` | the picked in-engine masters (checked in) | `make_shots.py` runs |
| `copy.lock.json` | the feature docs as the copy last covered them | `--reviewed` |

## Rebuilding

```powershell
python scripts/site/build_site.py            # facts + words + pictures -> docs/
python scripts/site/build_site.py --check    # what CI runs: exit 1 when docs/ is behind
```

The **facts** (car cards, circuit list and outlines, every number in the
text) are read from `content/` and the server source on each build, so a new
car, a renamed circuit or another setup knob needs only a rebuild. CI fails
a push that changed them without one.

## Refreshing the pictures

Car renders come from `content/props/_preview/cars/` (the Blender previews,
`scripts/content/cars/preview_cars.py`): re-render, rebuild.

In-engine pictures (needs the engine, a built editor target and a release
server build; a minute or so per shot, the game opens in a window):

```powershell
python scripts/site/make_shots.py                    # all of site/shots.yml
python scripts/site/make_shots.py rain garage        # some
python scripts/site/make_shots.py --pick             # after changing a `pick`
python scripts/site/build_site.py
```

Action shots are seeded AI races played back under the replay cameras with
no HUD; each keeps a few candidate frames in `out/site/candidates/<id>/`
(open `sheet.jpg`) and the shot's `pick` says which one is used. The races
replay bit for bit, so after a visual change the same pick is the same
moment.

## Refreshing the words

```powershell
python scripts/site/build_site.py --stale      # sections of the feature docs (docs/) changed since the last review
# ...rewrite site/copy.yml against them (in Claude Code: /site-refresh)...
python scripts/site/build_site.py --reviewed   # record them as covered
python scripts/site/build_site.py
```

Numbers are never typed into `copy.yml`: write `{{ n.cars }}`,
`{{ n.setup_knobs }}`, `{{ name('Spa') }}` (the header of `copy.yml` lists
them). Circuits appear by their display names only.
