# Verification

Features that are built and tested but have not been seen in the running
game, heard, or felt on real hardware. Nothing here is known to be wrong;
each item is a look, a listen or a session that has not happened yet.
Bugs and rough edges on built features are in [follow-ups.md](follow-ups.md),
features not started in the [roadmap](roadmap.md), simulation and
racing-rule gaps in [simulation-gaps.md](simulation-gaps.md). An item
lives in exactly one of the four.

The items themselves are in
[verification/verification-items.yml](verification/verification-items.yml),
one per check with a stable id, the run or sitting that checks it, its
evidence, the agent's verdict and the owner's review. The
[verifier](verifier.md) is the tool over it: `python
scripts/verify/verifier.py serve` is the review page, and its CLI is how
the agent adds, judges and closes items. This page holds only how the
checks are made.

**A new item** goes in the items file with an id (`verifier.py add`), never
as prose here: what to look at, `check_by` (the run, the sitting block, or
`human`), the doc that says what it should look like, and `needs` when it
waits on hardware. A shot in `scripts/verify/runs.yml` that is for an item
names it (`item:`, an id or a list) beside its free-text `checks`, so the
run attaches itself as evidence. After a run the agent judges the items it
shot (`verifier.py judge`, with the shot numbers); the owner confirms or
overrules on the page, and the agent processes the saved reviews when asked
(`verifier.py reviewed`): a confirmed item is closed, a "not right" becomes
a follow-up in the package that owns the feature, a "can't tell" gets a
better run.

## Unattended screen runs

Each run is one launch: a circuit, a sky and a camera plan, with every item
that fits that combination on it. A launch costs minutes (server, track
build, countdown) and a shot costs nothing, so a run brings each feature to
the camera at a known second rather than waiting for it: the server's debug
events with the stand-in driver ([server/operations.md](server/operations.md#debug-hooks):
`APEXSIM_DEBUG_STAND_IN_DRIVER=1`, `APEXSIM_DEBUG_EVENTS`),
`apexsim.car.DamagePreview`, and `apexsim.cam.LookAt` / `Goto` through
`-ApexExecAfter`, with `-ApexAutoRace`, `-ApexScreenshotAfter` and the other
switches in [game/client.md](game/client.md#unattended-runs-and-debugging).
`python scripts/verify/run_checks.py <run>` runs one: the recipes are in
`scripts/verify/runs.yml`, each against a server of its own on port 9400,
and the shots land in `out/verify/<run>/` (full size in `shots/`, tiled
2x2 into labelled contact sheets, listed in `shots.md`).
`scripts/verify/spots.py <Track>` works out camera poses from the track
YAML and scene (low cells, grades, bends, a prop seen from the road,
`--eye` a driver's eye on the road looking along the lap). Each shot is
judged against what the feature's doc says it draws.

Every circuit has a reference run of its start/finish, `sf-ref-<stem>`
(made from [cota-startfinish-comparison.md](cota-startfinish-comparison.md)):
the SF-26 on pole, sunny at 13:00, racing line off and the HUD hidden
(`apexsim.hud.Hide 1`), four shots before the lights so nothing moves: the
cockpit from the pole box, the chase `roof` rung there, a driver's eye on
the line down the lap and the same eye back over the grid. Road, wall,
stand, horizon, sky and gantry changes are judged on these frames on more
than one circuit (`run_checks.py "sf-ref-*"` for all of them).

## Sittings: hands, ears and hardware

Everything an unattended run cannot check: a real wheelbase, pedals, a
shifter, a pad and speakers. The wheel checks are run as the blocks of
[wheel-sitting.md](wheel-sitting.md) (`run_checks.py --sit <block>`, the
blocks in `scripts/verify/sittings.yml`); each block's checks are items of
kind `sitting`. Items that need something not on the rig (a second
machine, another GPU, a borrowed base) are `blocked` with their `needs`.

## Platforms and GPUs

Checks that need another machine ([building.md](building.md)): integrated
GPUs, Windows on Arm, a second client on the LAN. They are items of kind
`platform`.
