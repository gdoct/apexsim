# Verifier: a developer tool for clearing the verification backlog

## Why

The agent builds features faster than a person can look at them. What is
built and tested but not yet seen, heard or felt sits in
[docs/verification.md](../../apexsim/docs/verification.md) and in the
work plan's V2, V3, H1 and H2 rows, and the one scarce resource that
clears it is the owner's time. Today a check costs that time three ways:
finding the item's evidence (a run folder among 250 under `out/verify/`,
a shot number inside a 2x2 contact sheet), reading the prose to learn
what the agent thinks the shot shows, and then telling the agent the
verdict so it edits the docs. The tool collapses that to one keystroke
per item: the evidence and the agent's claim side by side, a decision,
next item.

The tool runs nothing and judges nothing. Runs are
`scripts/verify/run_checks.py`, judging is the agent's; the tool is where
a human confirms or overrules that judgement quickly, and the queue that
carries the verdicts back to the agent.

## The loop

1. The agent builds a feature and **registers an item**: what to look at,
   how it is checked, which doc says what it should look like.
2. A run or a sitting produces **evidence**: shots in `out/verify/<run>/`,
   marks and logs in `out/sit/<date>/<block>/`. The agent attaches the
   evidence to the item and writes its **verdict** ("LEAK and FLAT read
   at 60 s; the glazed patches are not readable at these poses") with the
   shot numbers it used.
3. The human opens the tool (daily, when no agent is running), walks
   the queue and **reviews** a batch: confirmed, not right, can't tell,
   or later, with a comment and a score. Reviews are held in the page
   until **Save** writes them all into the items file at once.
4. Outside the tool, the agent reads the reviewed items when asked and
   processes them into the work plan and the follow-ups: confirmed items
   leave the backlog, "not right" becomes a follow-up in the owning
   package, "can't tell" gets a better run. Not this tool's job.

The human never edits a doc, and the agent finds its feedback in one
structured file instead of scattered remarks.

## What changes in the backlog: a structured source

`verification.md` is prose grouped by run, with the agent's run notes
appended as paragraphs. It reads well and parses badly: shots name their
item by free text (`checks: "R1: FS on the front left in the tyre
panel"`), so nothing links a shot to a bullet reliably, and the same item
is often described in three places (the bullet, the run note, the work
plan). The tool needs stable ids, so the items move once into a structured
file, and from then on that file is the backlog:

- `apexsim/docs/verification/verification-items.yml` is the backlog. The agent adds,
  judges and closes items here (through the CLI below, or by hand within
  the schema); the tool writes the human's reviews into it. Both edit
  the same file, never at the same time: the tool runs when no agent
  does, and it refuses to save over a file that changed on disk since it
  was loaded.
- `apexsim/docs/verification.md` keeps its place in the docs index but
  holds only what is prose today: what verification is, how the runs and
  sittings work, the reference runs, and the rule that every new item
  goes in `verification-items.yml` with an id. No item lists; the tool is the view.
  CLAUDE.md's "Keeping the docs" row points at the file.
- `scripts/verify/runs.yml` shots carry `item:` ids (one or a list)
  beside the free-text `checks`, so `run_checks.py` registers a run's
  shots as evidence on its items when the run finishes, with the commit
  it ran at. `sittings.yml` check ids already exist (`checks: [{id,
  check, answers}]`); they become items of kind `sitting`, and a sitting's
  `marks.md` is their evidence.

### Item schema

```yaml
- id: v-r1-rain-light            # stable, never reused; v-<run or area>-<slug>
  title: The rain light in the wet
  area: cars                      # one of a fixed list (below)
  kind: screen | sitting | hardware | listen | platform
  check_by: r1-spa-wet            # run id, sitting block, or "human"
  created: 2026-10-20             # the day the feature landed
  commit: ee3c368f                # the feature's commit: evidence must be newer
  plan: V2                        # work-plan row, optional
  doc: game/client.md#rain-light  # what it should look like, per the feature's doc
  what: >-                        # the check, one or two sentences, from today's bullet
    The rain light (apexsim.car.RainLightNits) in the wet; a body with
    no car_rainlight slot has none.
  status: open | judged | confirmed | not-right | cant-tell | later | blocked
  needs: [wheel, speakers, second-pc, gpu-amd]   # hardware tags, optional
  evidence:                       # appended by run_checks.py and the agent
    - run: r1-spa-wet
      date: 2026-10-25
      commit: 73742bf9
      shots: [3, 4]               # out/verify/r1-spa-wet/shots/03.jpg ...
      kind: shot | sheet | marks | log | replay | video
      note: chase camera, 60 s, heavy rain
  verdict:                        # the agent's, replaced on each judge
    date: 2026-10-25
    by: agent
    says: looks-right | not-right | not-readable
    text: >-
      LEAK and FLAT read; the rain light is not readable at these poses.
    shots: [3]
  notes:                          # today's run paragraphs, dated
    - {date: 2026-10-25, text: "Run r1-spa-wet: ..."}
  review:                         # the human's; the only block the tool writes
    at: 2026-10-26T21:14:00
    score: 4                      # 1 broken, 2 wrong, 3 passable, 4 good, 5 ship it
    comment: "Too dim from the chase camera; fine in cockpit."
    shots: [{run: r1-spa-wet, shot: 3}]   # which evidence the review is about
    flag: true                    # "come back to this", optional
    processed: false              # the agent sets true once it has acted on it
```

`status` and `review` are the human's half: a review sets `status` to
`confirmed`, `not-right`, `cant-tell` or `later` and fills `review`. The
agent's half is everything else. `open` means no evidence yet, `judged`
means the agent has a verdict and the human has not looked, `blocked`
means hardware or a second machine is needed.

Areas are a fixed list so grouping is stable: `cars`, `tyres`, `track`,
`sky-weather`, `hud`, `menus`, `cameras`, `audio`, `ffb-input`, `ai`,
`race-rules`, `pit`, `spectator-replay`, `network`, `platform`. An item
has exactly one. The run (R1, R2...) is `check_by`, not the area: the
doc's grouping by run is how the agent clears items in batches, the
area is how the human wants to walk them, and both views come from the
same file.

### Review rules

The tool enforces these before a review is held for saving: `not-right`
and `cant-tell` need a comment (it is the follow-up's first line, or the
brief for the next run). `confirmed` with a score under 3 is refused:
that is a `not-right`. A `later` needs nothing and sorts the item to the
bottom until its evidence changes. A reviewed item that gets new
evidence goes back to `judged` and keeps its old review under `notes`,
so the second look sees what the first one said.

## The UI

One local web page, keyboard first, usable on a tablet beside the rig.

**Queue (home).** The default order is what pays off fastest, not date:
items with a fresh agent verdict and evidence first (one keystroke each),
then items with evidence but no verdict, then items waiting for a run,
then items that need hardware (`needs:`), each group collapsible and
each sorted by area then date within it. Filters as toggles, remembered
between visits: area, kind, status, run, `needs` tag, date range,
flagged, has evidence. Sort by date, area, run or score. A summary bar
reads open / judged / confirmed / not right per area and overall, how
many reviews are held unsaved, and how many saved reviews the agent has
not processed yet.

**Item.** The whole screen is evidence: the shots at full resolution,
not the 2x2 sheet, with the agent's verdict and the item's `what` and
`doc` link in a side column, the shot's label, time and run under each
frame. Where an item has evidence from two runs or two commits they show
side by side (before and after, two circuits). Logs and marks open
inline. Keys: `1-4` decide (confirmed / not right / can't tell / later),
`0-5` after a decision score it, `c` focuses the comment, `f` flags,
`j`/`k` or arrows move, `z` zooms the shot under the cursor to 1:1
pixels (the checks are often a tread face or a read-out), `o` copies the
command that opens the evidence in the game (`-ApexWatchReplay` with the
run's replay file) when the item has one. A held review shows at once in
the queue with a yellow mark until saved; an item the human flagged shows
red; a stale item (below) shows grey.

**Sitting mode.** The section 2 items for one block of `sittings.yml` as
a checklist with the block's answer words as big buttons, large type, no
mouse: the tablet beside the wheel, or the phone. The runner's
`--sit-sheet` output becomes this screen, and its MARK lines appear under
the block's checks after the sitting.

**Save.** Lists the held reviews grouped by outcome (confirmed, not
right, can't tell, later) and writes them into `items.yml` in one go,
each item's `status` and `review` block and nothing else, keeping the
file's order and comments. If the file changed on disk since the page
loaded (an agent was running after all) the save is refused and the page
reloads with the held reviews kept. Processing the saved reviews is a
request to the agent in a later session, outside the tool.

**Stale evidence.** An item whose evidence `commit` is older than its own
`commit` (the feature changed after the last shot) is marked stale and
sorted with the waiting-for-a-run group, the run to repeat named. Today's
doc does this by hand with "run again".

## CLI (what the agent uses)

`python scripts/verify/verifier.py <cmd>`, YAML in and out, no editor:

- `add` an item (fields as flags, or a YAML snippet on stdin)
- `evidence <item> --run <run> --shots 3,4` (also called by
  `run_checks.py` at the end of a run for every shot with `item:`)
- `judge <item> --says looks-right --text "..." --shots 3`
- `reviewed` lists the items with a saved review not yet processed, with
  their comments: what the agent reads when asked to process the feedback
- `close <item>` moves a processed item to `items.closed.yml`, kept for
  the record and the summary's totals
- `check` validates the file against the schema (ids unique, areas from
  the list, evidence paths present); it runs in the content checks
- `import` runs once: parses today's `verification.md` into items. The
  bullets become items, the "Run 2026-10-xx (...)" paragraphs become
  `notes`, the run headings give `check_by` and a first guess at `area`.
  The agent reviews the result; this is the only time the mapping is by
  hand.
- `serve [--lan]` starts the UI on localhost (or on the LAN for the
  tablet) and opens the browser

## Non-goals

- No video capture. The client takes stills; a replay is the video,
  opened in the game from the item's command. If the client learns to
  record clips, `kind: video` is already in the schema.
- No judging in the tool: no image diffing, no model. The agent judges
  in its run; the tool shows.
- No Claude integration, and no processing of the feedback: turning a
  "not right" into a follow-up or a work-plan row is the agent's job in a
  later session, from the `review` blocks in the file.
- No editing of items from the UI beyond `status` and `review`; the
  agent owns the rest of the item, so the two halves never collide.
- No accounts, no database, no network beyond the LAN option.

## Tech stack

**Python 3.11 with the standard library's `http.server`, and one
vanilla HTML/CSS/JS page.** Nothing to install, nothing to build, one
command to start.

Why this and not the alternatives:

- The verify tooling is Python already (`run_checks.py`, `spots.py`,
  `cvd.py`; PyYAML and Pillow in use). The CLI and the server share the
  schema code, and `run_checks.py` imports it to register evidence.
  Thumbnails are made with Pillow on first request and cached beside the
  shots (`out/verify/<run>/thumbs/`), so the queue stays fast over 250
  run folders of 1920x1080 JPEGs. The server is a few hundred lines; the
  stdlib server is enough for one user on localhost and a tablet.
- The server's dashboard (`server/server/src/admin/web/`) is one
  `index.html`, `app.js` and `app.css` with no framework, dark, and
  already looks like ApexSim. The verifier page copies its CSS tokens and
  idiom, so in-house tools share one style and the repo gets no second
  front-end toolchain. A page with a queue, a viewer and a checklist does
  not need React or a bundler; a Node toolchain for one user is not
  repaid. Node 24 is on the machine if that ever changes.
- Not Rust in the server: the verifier is a developer tool over files on
  one machine, not a server feature; building it into the authoritative
  server couples the backlog to the game's binary and its release cadence.
- Not a static page with the File System Access API: Chrome only, picks
  folders again each launch, and cannot be reached from a tablet.
- Not Electron or Tauri: a browser tab is enough and the LAN option
  comes free with a server.

Shape: `scripts/verify/verifier.py` (CLI and server, about the size of
`run_checks.py`) and `scripts/verify/verifier/` (`schema.py` for the
YAML model and validation, `web/index.html`, `app.js`, `app.css`).
Endpoints: `GET /api/items`, `GET /api/runs/<run>` (the shot list),
`GET /thumb/<run>/<n>`, `GET /shot/<run>/<n>`, `GET
/log/<run>/<client|server>`, `POST /api/reviews` (the held reviews;
writes `items.yml` atomically after the changed-on-disk check, touching
only `status` and `review`). Port 9410, beside
the verify runs' 9400-9403. Start: `python scripts/verify/verifier.py
serve`.

Where the code lives: under `apexsim/scripts/verify/`, with the runner it
extends, not in a root `tools/` folder (code is `apexsim/`, by the three
roots rule). This spec moves to `apexsim/docs/verifier.md` when the tool
is built, and `building.md` names it.

## Build order

1. Schema, `import`, `check`: the structured file exists, the items are
   in it once, `verification.md` shrinks to the prose and points at it,
   CLAUDE.md's docs rule names the file. No UI yet; this alone gives
   stable ids and lets `run_checks.py` attach evidence.
2. `item:` ids on the shots in `runs.yml` that already map (the C5,
   roadmap 38, BLUE FLAG and R-series shots: 1467 `checks` lines, most
   by pattern).
3. `serve` with the queue, the item viewer and Save; `reviewed` and
   `close` for the agent.
4. Sitting mode on the tablet, the LAN option.
5. Stale marking, before/after pairing, the open-in-game command.
