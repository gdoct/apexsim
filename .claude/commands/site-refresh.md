---
description: Bring the marketing page (docs/index.html) up to date with the game
---

Refresh the marketing page. Its source is `site/`, its generator
`scripts/site/build_site.py`; read `site/README.md` first.

1. Run `python scripts/site/build_site.py --stale`. Each line is a section of
   a feature doc under docs/ that is new or changed since the copy was last
   reviewed. Read those sections.
2. Update `site/copy.yml` so the page tells what the game now does: revise a
   feature card whose subject changed, add one for a major new feature, drop
   a claim that stopped being true. Keep the voice of the existing copy:
   concrete, short, no superlatives. Check each claim against the code or
   the doc before writing it, and against docs/simulation-gaps.md and
   docs/roadmap.md so the page promises nothing that is deferred.
   Never type a number the build can supply (`{{ n.* }}`), and name circuits
   only through `{{ name('<Stem>') }}`.
   The feature grid is laid out in rows of two wide cards or three normal
   ones: keep the card count such that no row is left with a hole.
3. If a new feature deserves a picture, add a shot to `site/shots.yml` and an
   entry to `site/media.yml`, and say that `python scripts/site/make_shots.py
   <id>` has to be run (it opens the game); only run it yourself when asked.
4. `python scripts/site/build_site.py`, then look at the page (a headless
   browser screenshot of `docs/index.html`) before calling it done.
5. `python scripts/site/build_site.py --reviewed`, then
   `python scripts/site/build_site.py --check` and
   `python -m unittest discover -s scripts/site/tests`.

Report what changed on the page and what you left alone. Do not commit.
