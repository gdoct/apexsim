---
description: Bring the marketing site (docs/*.html) up to date with the game
---

Refresh the marketing site. Its source is `site/`, its generator
`scripts/site/build_site.py`; read `site/README.md` first. Every path here
is under `apexsim/` (the code root): run the commands from there. The facts
and pictures come from `../apexsim-content`, and the content docs the copy is
also written against are in `../apexsim-content/docs`.

1. Run `python scripts/site/build_site.py --stale`. Each line is a section of
   a feature doc under docs/ that is new or changed since the copy was last
   reviewed. Read those sections.
2. Update `site/copy/*.yml` so the site tells what the game now does: the
   detail goes on its page (`simulation.yml` for anything simulated, a
   chapter or a point; `modding.yml`, `ac-import.yml`, `faq.yml`,
   `requirements.yml`), and the front page (`index.yml`) only gains a tile
   or a line for a major feature: keep it short, no text blocks there. Drop
   a claim that stopped being true. Keep the voice of the existing copy:
   concrete, short, no superlatives. Check each claim against the code or
   the doc before writing it, and against docs/simulation-gaps.md,
   docs/follow-ups.md and docs/roadmap.md so the page promises nothing that is deferred.
   Never type a number the build can supply (`{{ n.* }}`), and name circuits
   only through `{{ name('<Stem>') }}`.
   The front page's tiles sit four to a row: keep their count a multiple
   of four. Assetto Corsa is named as plain text only, never with its art.
3. If a new feature deserves a picture, add a shot to `site/shots.yml` and an
   entry to `site/media.yml`, and say that `python scripts/site/make_shots.py
   <id>` has to be run (it opens the game); only run it yourself when asked.
4. `python scripts/site/build_site.py`, then look at the page (a headless
   browser screenshot of each changed page under `docs/`, at desktop and
   phone width) before calling it done.
5. `python scripts/site/build_site.py --reviewed`, then
   `python scripts/site/build_site.py --check` and
   `python -m unittest discover -s scripts/site/tests`.

Report what changed on the site and what you left alone. Do not commit.
