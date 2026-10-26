# The marketing site's source

The pages under `docs/` (`index.html` and one `.html` per page, published by
GitHub Pages) are generated from this folder. Nothing under `docs/*.html` or
`docs/assets/` is edited by hand.

| File | What it holds | Changes when |
|---|---|---|
| `copy/<page>.yml` | a page's words: `index` (the front page, kept short), `simulation`, `cars`, `circuits`, `ac-import`, `modding`, `faq`, `requirements` | the game gains a feature worth telling |
| `copy/common.yml` | what every page shares: the menu, the class blurbs, the download call, the Assetto Corsa disclaimer, the footer | a page is added, or the shared words change |
| `templates/<page>.html` | a page's structure, on `base.html` (head, menu, footer) and the pieces in `parts.html` | the design changes |
| `site.css`, `site.js` | the look and behaviour of every page; each part of the script runs only where its elements are | the design changes |
| `media.yml` | which picture goes where, cut from which file in the repo | a picture is added or swapped |
| `shots.yml` | the in-engine shot list | a shot is added, or its camera or `pick` changes |
| `shots/` | the picked in-engine masters (checked in) | `make_shots.py` runs |
| `copy.lock.json` | the feature docs as the copy last covered them | `--reviewed` |

A page is a `copy/<page>.yml` with a `templates/<page>.html` of the same
name; the build renders every one into `docs/<page>.html` and removes a page
that is gone. Link between pages with `{{ link('faq') }}` or
`{{ link('simulation#tyres') }}`: a link to a page that does not exist fails
the build, and a test checks that every `#fragment` lands on an id.

## Rebuilding

```powershell
python scripts/site/build_site.py            # facts + words + pictures -> docs/
python scripts/site/build_site.py --check    # what CI runs: exit 1 when docs/ is behind
```

The **facts** (car cards, circuit list and outlines, every number in the
text) are read from `../apexsim-content` and the server source on each build, so a new
car, a renamed circuit or another setup knob needs only a rebuild. CI fails
a push that changed them without one.

## Refreshing the pictures

Car renders come from `apexsim-content/props/_preview/cars/` (the Blender previews,
`apexsim-content/tools/generators/cars/preview_cars.py`): re-render, rebuild.

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
python scripts/site/build_site.py --stale      # sections of the feature docs (docs/, ../apexsim-content/docs) changed since the last review
# ...rewrite site/copy/*.yml against them (in Claude Code: /site-refresh)...
python scripts/site/build_site.py --reviewed   # record them as covered
python scripts/site/build_site.py
```

Numbers are never typed into the copy: write `{{ n.cars }}`,
`{{ n.setup_knobs }}`, `{{ name('SpaFrankenchamps') }}` (the header of
`copy/common.yml` lists them). Circuits appear by their display names only.
The detail belongs on its page; the front page gets a line and a link.
