# Verifier: the verification backlog and its review page

The agent builds features faster than a person can look at them. What is
built and tested but not yet seen, heard or felt is the verification
backlog, and the one scarce resource that clears it is the owner's time.
The verifier puts the evidence (the run's shots at full size, its logs) and
the agent's claim about it side by side, takes a decision in one keystroke,
and carries the verdicts back to the agent in one structured file.

The tool runs nothing and judges nothing. Runs are
`scripts/verify/run_checks.py` ([verification.md](verification.md)),
judging is the agent's; the verifier is where a human confirms or overrules
that judgement, and the queue that carries the verdicts back.

## The loop

1. The agent builds a feature and **adds an item**: what to look at, how it
   is checked, which doc says what it should look like
   (`verifier.py add`).
2. A run or a sitting produces **evidence**: shots in `out/verify/<run>/`,
   marks and logs in `out/sit/<date>/<block>/`. `run_checks.py` attaches a
   run's shots to the items their `item:` names when the run ends; the
   agent attaches any other (`verifier.py evidence`) and writes its
   **verdict** with the shot numbers it used (`verifier.py judge`).
3. The human opens the page (`verifier.py serve`, when no agent is
   running), walks the queue and **reviews** a batch: confirmed, not
   right, can't tell or later, with a comment and a score. Reviews are held
   in the page until **Save** writes them all into the items file at once.
4. Outside the tool, the agent reads the saved reviews when asked
   (`verifier.py reviewed`) and acts on them: a confirmed item is closed, a
   "not right" becomes a follow-up in the owning package, a "can't tell"
   gets a better run. It marks each as processed (`verifier.py processed`)
   and closes what is done (`verifier.py close`).

The human never edits a doc, and the agent finds its feedback in one file.

## The items file

`docs/verification/verification-items.yml` is the backlog; closed items move
to `verification-items.closed.yml` beside it, kept for the record and the
totals. Both writers edit the items file, never at once: the agent through
the CLI (or by hand within the schema), the page through Save. The page
runs when no agent does, and Save is refused when the file changed on disk
since the page loaded it.

Every write edits one top-level key of one item as text
(`verifier/schema.py`, `ItemsFile.set`), so the file's order, its comments
and every other key stay byte for byte as they were.

```yaml
- id: v-r1-rain-light            # stable, never reused; v-<run or area>-<slug>
  title: The rain light in the wet
  area: cars                      # one of the areas below
  kind: screen                    # screen | sitting | hardware | listen | platform
  check_by: r1-spa-wet            # run id, sitting block, or "human"
  created: 2026-10-14             # the day the feature landed
  commit: 01bcebd4                # the feature's commit
  plan: V2                        # work-plan row, optional
  doc: game/client.md#rain-light  # under docs/: what it should look like
  what: >-                        # the check, one or two sentences
    The rain light (`apexsim.car.RainLightNits`) in the wet; a body with no
    `car_rainlight` slot has none.
  status: judged                  # open | judged | confirmed | not-right | cant-tell | later | blocked
  needs: [wheel]                  # hardware tags, optional
  evidence:                       # appended by run_checks.py and the agent
  - run: r1-spa-wet
    date: 2026-10-25
    commit: 73742bf9
    shots: [3, 4]                 # none: the whole run
    kind: shot                    # shot | sheet | marks | log | replay | video
    note: chase camera, 60 s, heavy rain
  verdict:                        # the agent's, replaced on each judge
    date: 2026-10-25
    by: agent
    says: not-readable            # looks-right | not-right | not-readable
    text: The rain light is not readable at these poses.
    shots: [3]
  notes:                          # earlier run notes and old reviews, dated
  - {date: 2026-10-25, text: "Run r1-spa-wet: ..."}
  review:                         # the human's; the only block the page writes
    at: 2026-10-26T21:14:00
    score: 4                      # 1 broken, 2 wrong, 3 passable, 4 good, 5 ship it
    comment: Too dim from the chase camera; fine in cockpit.
    shots: [{run: r1-spa-wet, shot: 3}]   # what the review is about
    flag: true                    # come back to this
    processed: false              # the agent sets true once it has acted
```

`status` and `review` are the human's half; the agent owns everything else.
`open` is no verdict yet, `judged` an agent verdict the human has not
looked at, `blocked` a check that waits on hardware or a second machine.
A review sets `confirmed`, `not-right`, `cant-tell` or `later`. A
sitting answer is a review with `answer:` (the word tapped): the check's
first answer word in `sittings.yml` is `confirmed`, any other is
`not-right` with the word, and the note after it, as the comment
(`schema.answer_review`).

Areas are a fixed list so grouping is stable: `cars`, `tyres`, `track`,
`sky-weather`, `hud`, `menus`, `cameras`, `audio`, `ffb-input`, `ai`,
`race-rules`, `pit`, `spectator-replay`, `network`, `platform`. The run is
`check_by`, not the area: the agent clears items by run, the human walks
them by area. `needs` tags in use: `wheel`, `pad`, `speakers`, `by-hand`
(a person at the keyboard, no special hardware), `second-pc`,
`hdr-display`, `gpu-integrated`, `arm-pc`, `motec`, `simhub`,
`pxn-wheel`, `packaged-build`, `network-off`, `gameinput-plugin`.

The sitting checks of `scripts/verify/sittings.yml` are items of kind
`sitting` (`v-sit-<block>-<check>`), `check_by` their block.

### Rules

Enforced by the page and again by the server before a review is written:
`not-right` and `cant-tell` need a comment (the follow-up's first line, or
the brief for the next run); `confirmed` with a score under 3 is refused
(that is a `not-right`). `later` needs nothing and sorts the item to the
bottom until its evidence changes.

New evidence on a reviewed item sends it back to `judged` (`open` without
a verdict) and moves the old review into `notes`, so the second look sees
what the first said. A verdict older than the item's newest evidence
is shown as such and does not put the item in the ready group.

Evidence is **stale** when it was shot before the item's own `commit`: the
feature changed since (`schema.stale`: the evidence's commit does not
contain the item's, by `git merge-base --is-ancestor`; without both
commits, the evidence's date against `created`). A stale item sorts with
*waiting for a run*, greyed, the run to repeat named. Moving an item's
`commit` forward after changing its feature is how the agent asks for a
new look.

## The page

`python scripts/verify/verifier.py serve` serves one local page on
http://localhost:9410 (beside the verify runs' 9400-9403) and opens the
browser. `--items <file>` serves another items file, such as a scratch copy
to try the page on. Keyboard first; the layout folds to one column below
1000 px for a tablet.

**Queue.** Groups in the order that pays off fastest: *ready to review* (an
agent verdict on the latest evidence: one keystroke each), *evidence, no
verdict*, *waiting for a run*, *needs hardware or hands*, then the saved
*reviewed* and *later* (folded). Within a group: area, then date. The area
tiles count open, judged, confirmed and not right per area and filter by
area; the filters (kind, status, `needs`, run or block, created dates,
flagged, has evidence) and the sort (date, area, run, score) are kept in
the browser between visits. The top bar counts the statuses, the closed
items and the saved reviews the agent has not processed. A held review
marks its row amber, a flag red.

**Item.** The evidence fills the screen: each run's chosen shots at full
size (the rest of the run behind a button), newest run first, each with its
number, time, label and the `checks` line from `shots.md`; the verdict's
shots outlined. The run's client, notes and server logs open inline. The
side column has the title, the `what`, the doc (`/doc?p=` serves it as
text), the verdict, the notes and the review.

| Key | |
|---|---|
| `1` `2` `3` `4` | confirmed, not right, can't tell, later (not right and can't tell focus the comment) |
| `0`-`5` | right after a decision: the score (0 none); a valid review moves to the next item |
| `c` | the comment (Enter or Esc leaves it) |
| `f` | flag |
| `s` | mark the current shot as what the review is about |
| `u` | drop the held review |
| `←` `→` | the shot cursor |
| `z` | the shot under the mouse (or the cursor) at 1:1, centred where the mouse was |
| `b` | before and after: the newest evidence beside the one before it (another run, or the same run at another commit), shot for shot for one run, in order across two; with one piece of evidence, its run's previous generation |
| `o` | copy the command that opens the run's newest kept replay (`out/verify/<run>/replays/`) in the game's watch view: `play_editor.ps1 -NoServer -ApexNoDemo -ApexWatchReplay=<file>` |
| `j` `k` `↑` `↓` | next and previous item in the queue's order |
| `Esc` | back to the queue |
| `Ctrl+S` | the Save page |

**Sitting mode** (`#/sit`, SITTING in the top bar). The blocks of
`scripts/verify/sittings.yml`, each with its answered count; a block opens
as a checklist: each check in large type with its answer words as big
buttons (the first green when tapped, the others red) and a note line for
the MARK number. `↑` `↓` move between checks, `1`-`9` answer the one under
the cursor and step on, `n` the note. The answers are held reviews and
are written by Save like any other. Under the checks, the block's MARK and
force lines from the newest `out/sit/<date>/<block>/marks.md`, once the
sitting has been run. The game's verify card (below) answers the same checks from the
rim and needs no page. For a tablet beside the wheel, `serve --lan` listens
on every address and prints the LAN URL; anyone on the network can then
save reviews, so it is for the sitting only.

**Save.** The held reviews by outcome, the unfinished ones (no decision, a
missing comment) apart and left out. Enter writes each item's `status` and
`review` and nothing else. When the file changed on disk the save is
refused, the page reloads the file and keeps the held reviews. Held reviews
live in the browser's storage too, so a reload keeps them.

Endpoints: `GET /api/items` (the items, the file's digest, and the stale
items with the run to repeat), `GET /api/sittings` (the blocks, their
checks' items and answer words, their newest marks),
`GET /api/runs/<run>[?commit=|?gen=]` (its shots from `shots.md` at that commit or kept generation,
its logs, its history, its replay command),
`GET /thumb/<run>/<n>[?commit=|?gen=]` (480 px, cached beside the shots),
`GET /shot/<run>/<n>[?commit=|?gen=]`, `GET /log/<run>/<client|notes|server>` (the last
400 KB; `?all` for the whole), `GET /doc?p=<path>`, `POST /api/reviews`
(`{digest, reviews: [{id, status, review} | {id, answer, note, at, flag}]}`:
409 when the digest is not the file's; an answer not among the check's
words is a 400). The page is `scripts/verify/verifier/web/` with the server
dashboard's tokens and fonts (served from `server/server/src/admin/web/fonts`).

## The verify card (in the game)

A sitting is answered in the game, from the rim, so a human at the wheel
needs neither the page nor a keyboard. `python scripts/verify/run_checks.py
--sit <block>...` (or `--sit --open`: every block with a check still
`open`, `judged`, `cant-tell` or `later`) runs the blocks one after the
other, each its own launch with its own server, as before; it writes the
block's card to `out/sit/<date>/<block>/verify.json` and starts the game
with `-ApexVerify=<that file>`. `--minutes N` sets every block's driving
time (default: the block's `minutes`); `--no-card` leaves the answers to
the page.

The card holds the block's `about`, `setup` and `minutes`, whether it is
a race (`-ApexAutoRace` in its `args`), and its checks
(`schema.game_checks`): the block's own `v-sit-*` checks with their answer
words, then every other open item whose `check_by` is the block (its title
as the check, its `what` as the detail, answered OK / not right / can't
tell). Each carries the answer it was last given. A check whose item is
closed is left out, so `--sit --open` skips a block whose checks are all
closed; a re-sit of some rows is a block of its own.

In the game (`UI/ApexVerifyWidget.cpp`, `Verify/ApexVerifyCard.h`):

- **Brief and clock.** When the race view comes up (at once for a block
  with no race, such as the bench), the block's brief and its checks stand
  on the left for 45 s, and a clock at the top right counts the driving
  time down. At zero it reads TIME'S UP.
- **Opening the card.** At any time, without waiting for the clock: the
  pause key (the wheel's, the pad's Start, the keyboard's; in the race, in
  the menus and in settings, but not a key that is also the menus' Back)
  opens the card in place of the pause menu, and the clock's ANSWER NOW
  takes a click. The card's PAUSE MENU button reaches the pause menu
  itself (its VERIFY row leads back). In a block with no race (the bench,
  which starts in settings), leaving settings the first time opens the
  card. It also opens by itself when the time is up and nothing is being
  driven, or when a race block's race view has gone (the race finished,
  the session left). While it is open the car's controls are off, as
  under the pause menu, and the clock stands still.
- **One check at a time.** The check in large type, its detail, the
  answer it was given before, and its answer words as buttons. Picking one
  moves to the next check. NOTE opens an on-screen keyboard (letters,
  digits, Shift, Space, Delete, Clear, Done) walked with the wheel's menu
  directions; a real keyboard types into the note too, and typing on the
  card opens it. FLAG marks the check to come back to. BACK and SKIP step
  through the checks. DRIVE ON (or the pause key) closes the card and
  returns to the car.
- **The end.** Every answer and note in a list, then NEXT BLOCK, STOP
  HERE, 3 MORE MINUTES (back to driving with the clock wound on), or BACK.
  Next and stop write the answers file and quit the game.

The answers file (`verify-answers.json` beside the card, `{block, next,
answers: [{item, answer, note, flag, at}]}`) is rewritten on every
change, so a game closed half way keeps what was answered (with `next:
stop`). When the game exits, the runner turns it into reviews
(`schema.apply_game_answers`): a sitting check's answer as on the page
(`answer_review`), a hand item's OK as `confirmed`, not right as
`not-right` and can't tell as `cant-tell`, the word as the comment. A
note or a flag with no answer is a `later`. The runner saves straight
into the items file, so the page should not be open with held reviews
during a sitting: its Save would be refused (the digest changed) and it
reloads. Then the next block starts, unless the player chose STOP HERE or
closed the game.

`apexsim.verify open|close|answer <n>|note|type <text>|flag|skip|back|more|next|stop`
drives the card from the console, for an unattended look
(`--game-arg=-ApexExecAfter=...` on the runner).

## CLI

`python scripts/verify/verifier.py <cmd>`, run from `apexsim/`:

- `check [--no-paths] [--warnings]` validates the file: ids unique (closed
  ones too) and shaped `v-...`, areas, kinds, statuses and verdicts from
  their lists, the review rules, and the evidence on disk (warnings: `out/`
  is not in git). CI runs it with `--no-paths`.
- `add` an item from flags (`--id --title --area --kind --check-by --what
  --doc --plan --needs --blocked`), or `add -` with a YAML item or list on
  stdin. `created` and `commit` default to today and HEAD.
- `evidence <item>... --run <run> [--shots 3,4] [--kind] [--note]`: the
  same run on the same day replaces its earlier entry.
- `judge <item> --says looks-right|not-right|not-readable --text "..." [--shots 3]`.
- `reviewed [--all] [--yaml]` lists the saved reviews not yet processed,
  with their comments: what the agent reads when asked to process the
  feedback.
- `processed <item>...` marks reviews as acted on; `close <item>...` moves
  items to the closed file (`--force` for an unprocessed review).
- `import` made the file from the prose `verification.md` and
  `sittings.yml` once (2026-10-25); it refuses to run over a file with
  items.

`run_checks.py` calls `schema.register_run` after every run (not under
`--repeat`): each taken shot whose recipe in `runs.yml` has `item:` (an id
or a list) becomes evidence on that item, with HEAD's commit. Before a run
shoots again, its last shots (and `shots.md`) move to
`out/verify/<run>/history/<date>T<time>-<commit>/` (`schema.keep_previous`,
the newest three kept), and `run.json` records the commit and the minute of
the new ones: evidence names a commit, and the page finds that
generation's shots whether they are the run's latest or kept. Before and
after with one piece of evidence takes the generation before it by its
folder (`?gen=`), so two runs on one uncommitted change compare too.

## Checks

`python -m unittest discover -s scripts/verify/tests` (in CI with the site
job): a review changes only `status` and `review` and keeps the comments,
the rules, the changed-on-disk refusal, new evidence reopening a review,
`register_run`, the server's POST, a sitting answer's mapping and its
POST, every sitting check having an item, the stale rule, the kept shot
generations, and the backlog itself valid.

## Not in the tool

- No video capture: a replay is the video, opened in the game.
- No judging: no image diffing, no model. The agent judges in its run.
- No processing of the feedback, and no editing of items beyond `status`
  and `review`.
- No accounts, no database. The page listens on localhost only, or with
  `--lan` on the local network for a sitting.

Python's standard `http.server` and one plain HTML/CSS/JS page: nothing to
install beyond the PyYAML and Pillow the verify scripts use, nothing to
build, and the schema code shared by the CLI, the server and
`run_checks.py`.
