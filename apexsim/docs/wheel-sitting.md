# Wheel sitting plan

Every check that needs hands on a wheel, done in as few sittings as possible.
It covers work-plan item H1, the wheel half of H2 and the wheel items of
the verification backlog ([verification.md](verification.md)). The items
file stays the source of truth for *what* is unverified (each block's
checks are its `v-sit-<block>-<check>` items). This file says *how* to
clear it in one go: what an agent prepares beforehand, the order the human
drives in, what gets recorded on its own, and what the agent does with the
results afterwards.

The rig is the Fanatec ClubSport V2.5 (COL01 forces and steering, COL02
pedals and buttons) on the dev machine. Checks that need a different base go
in [Borrowed hardware](#borrowed-hardware) at the end.

## Principles

- **The human only drives and judges.** Building, launching, setting up a
  session, firing a puncture or bending a suspension, and reading logs are
  agent work. Each block is one command that opens the game straight on the
  track, already set up.
- **Record, don't describe.** A wheel button marks the moment something feels
  wrong. The game logs what the car and the mixer were doing around each mark,
  so a finding is a timestamp plus a word, not a paragraph.
- **One launch per block, blocks ordered by setup.** Same track and car
  back to back, events timed so they never overlap.
- **Answer with fixed words.** Each check has fixed answers (OK / weak /
  strong / late / wrong / not felt) and room for one line. The agent turns
  those into follow-ups without a second round of questions.
- **Re-sit only what changed.** After tuning, the next sitting covers only
  the rows the tuning touched.

## Phase 0 — agent prep (built)

All 🟢; what is in place before a sitting:

| # | Prep | Where |
|---|---|---|
| P1 | **Sitting runner**: `python scripts/verify/run_checks.py --sit <block>` starts the block's own server (port 9400, its `server.events`), launches the game with the block's `args` and `exec`, the player's own display settings and save, no stand-in, no shots and no time limit. Before launching it prints the block's setup and checks (and puts a paste line on the clipboard); when the game closes or is killed it gathers the client and server logs, new `Saved/Marks` CSVs and `Saved/Telemetry` lap traces into `out/sit/<date>/<block>/`, with `marks.md` listing every MARK and 15 s force line. The save games are copied to `out/sit/<date>/SaveGames.bak` once per day. `--sit --list` lists the blocks, `--dry-run` prints the command line. | `scripts/verify/sittings.yml`, `run_checks.py` |
| P2 | **MARK**: the `Mark` action (K on the keyboard; pad and wheel slots unbound, bound in block 0) and `apexsim.debug.Mark [word]`. A press logs `MARK n "word": ...` with speed, gear, station, steer and rim angle, and each signal's mean and range over the last 2 s (torque, constant force, correction, stiffness limit, road, vibration, damper, pad motors, slides, kerb, off track, heat, damage, flat spot), writes the frames to `Saved/Marks/<run>-<n>-<word>.csv`, plays the Notice cue and shows `MARK n` on screen. | `ApexFfbMarks`, `AApexPlayerController::Mark`; test `ApexSim.Input.ForceFeedback.Marks` |
| P3 | **Tyre heat event**: `heat=FL:C` (or `front`, `rear`, `all`) sets the tread, its zones and the core to C °C. | `server/sim/src/debug_hooks.rs`; `a_heat_event_cooks_the_fronts` |
| P4 | **Front-axle hybrid car**: the AC 919 Hybrid 2016 imported as `apexsim-content/cars/custom/KsPorsche919Hybrid2016` with `[hybrid] axle = "front"` (300 kW, 8 MJ a lap). `imported_car_test` and `car_stability_test imported` pass; the game loads it at runtime. Custom content: re-import on another machine. | block F, `-ApexCar=919` |
| P5 | **Toe-in setup line**: five clicks of toe-in on both axles of the Caravan GT3, and a line for another car to see refused. | block G's `paste` |
| P6 | **Answer sheet**: `python scripts/verify/run_checks.py --sit-sheet` writes `out/sit/<date>/sheet.md`, one row per check with its answer words. | `--sit-sheet` |
| P7 | **Builds**: the physics library and `ApexSimEditor` (built with `-DisableAdaptiveUnity`), the release server in `server/server/target`. `ApexSim.Input.*`, `ApexSim.Prediction.Feel.*` and the car TOML / GLB tests pass. Rebuild after any later change: the runner warns when a DLL or the server is older than its source. | |
| P8 | **Device check**: no device records exist on the machine yet (`Saved/ApexInputDevices.json`), and the wheel was off when prep ran. Block 0 lists the devices 2 s in (`apexsim.input.Devices`) and the bindings are made there. | block 0 |

The human's only prep: the Fanatec App shows no pending firmware (a driver
update without firmware silently drops game forces; see
[game/input-and-ffb.md](game/input-and-ffb.md#traps)). Write down the base's
FF / NDP / NFR / SEN menu values on the sheet at the start.

## Phase 1 — the sitting

About 2 h 15 min of driving, best split into **Sitting 1** (blocks 0–C,
core feel, ~70 min) and **Sitting 2** (blocks D–I, conditions and extras,
~65 min), with the agent's first pass in between: if Sitting 1 finds the
base feel wrong, Sitting 2's conditions would be judged on a wrong baseline.

Every block is one command, `python scripts/verify/run_checks.py --sit
<block>` (ids in `scripts/verify/sittings.yml`: `0-bench`, `a-monza`,
`b-predict`, `c-nordschleife`, `c-mandarina`, `d-sakhir`, `e-spa-wet`,
`f-hybrid`, `g-tread`, `h-pad`, `i-imported-pit`). The human drives,
presses MARK (saying the word aloud or noting it on the sheet), closes the
game, and fills in the block's rows: on the sheet, or tapped in the
verifier's sitting mode on a tablet beside the wheel (`python
scripts/verify/verifier.py serve --lan`, then the printed URL;
[verifier.md](verifier.md)), where each answer is held as the check's
review and Save writes them all. Every block's `Wheel forces over 15 s
driving:` lines are kept.

### Sitting 1 — core feel

**Block 0. Bench, no driving (10 min).** Opens on the settings overlay,
Wheel tab (`-ApexOpenSettings -ApexSettingsTab=…`).
- Wheel rotation set to the base's SEN (910, not 240).
- Meters: right reads positive; pedals 0→1 without a dead band.
- Bind: `Clutch`, `HGearReverse`/`HGear1..8` (if a shifter is attached),
  `Handbrake`, `BoxThisLap`, `Mark`, the `Menu*` slots on the rim's buttons
  and hat.
- Direction test pushes right, at a sensible strength.
- Menus driven from the rim alone: back to the main menu and into the
  settings again.
- Wheelbase stepper shows Auto = ClubSport V2.5, 8 Nm.

**Block A. Baseline, Monza, GT3, dry, no latency (20 min).** 3 laps alone,
then the field joins (`-ApexAiCount=11`) for the rest. The reference block,
judged before anything else.
- Weight in a corner, the rim going light at the limit (understeer cue on
  time / early / late), letting go of the rim mid-corner (returns, rings,
  holds), braking felt through the rim, road texture on a straight,
  kerbs, the soft lock at full lock, centring at the start.
- No notch or tick from the constant force's 0.15 s restart.
- Gravel: the first chicane straight on. How hard it bogs, whether it can
  be driven out, the rattle through the rim, how it sounds.
- Steering lock Auto: the rim on screen (cockpit view) turns as far as the
  hands.
- The clutch's throw and torque cut, the H-shifter (if attached), from a
  standing start in the pit lane.
- `BoxThisLap` from the rim: the BOX badge on, off and on, one stop.
- Racing the AI (H2): room left alongside into the Rettifilo and Roggia,
  one defending move into the inside, not two.

**Block B. Client prediction at 80 ms, Monza, GT3 (15 min).** Same car and
track as A, so the comparison is fair. `-ApexPredict
-ApexFakeLatencyMs=80`, 3 laps; then `apexsim.net.Predict 0` from the
console, 2 laps; then back on for 1 lap.
- Answer to the wheel at once vs the puppet; corrections showing (twitch on
  a brake stab or kerb, a snapping slide, cockpit shake).
- The rim as heavy as A, understeer cue on time, no grain from rewinds,
  `steer_stiffness` helping or overshooting.
- Through the pit lane once: a thump or engine-note step at the switch to
  and from the puppet.
- Contact with an AI car once (one blended correction).

**Block C. Nordschleife, then Mandarina Bay (25 min).** One lap each in the
rally car (`lancha-037-rally`, H-shifter and handbrake), then Mandarina Bay
in the GT3.
- Sausage kerbs on both; the posts and grade plates read from the cockpit.
- A banked turn (Karussell) as extra grip; a diagonal crest (Flugplatz,
  Pflanzgarten) going light.
- Handbrake: how fast the tail comes round (`HANDBRAKE_SHARE`), in a
  Mandarina Bay hairpin.
- Safety, last thing in the sitting: mid-corner with load on the rim, kill
  the game in Task Manager. The force must drop within ~0.5 s.

**Agent between sittings:** the first pass of Phase 2 on blocks 0–C. If the
baseline needs tuning, tune it and re-sit block A's rows that moved before
Sitting 2.

### Sitting 2 — conditions and extras

**Block D. Car condition, Sakhir, GT3 (20 min).** Events timed by the
runner, one lap apart, so each is felt alone:
`heat=front` cooked fronts → `flatspot=FL:0.7` → `leak=FR:<slow>` (slow
puncture over a lap) → `damage=left:40` (bends that side) →
`damage=engine:30`.
The HUD names each one as it lands.
- Greasy fronts: the rim lighter, scrub lower.
- Flat spot: the thump once per turn through the rim.
- Slow puncture felt growing over a lap.
- Bent suspension: the wobble once per wheel turn, the pull to the damaged side.
- Engine: the half-order shake while it runs.
- Sakhir's desert floor off the track: the fine buzz, the slow drag.

**Block E. Wet, Spa, GT3 (15 min).** `-ApexWeather=heavyrain` with a
forecast drying to light rain (`-ApexForecast`).
- Puddles felt through the seat and rim (positions from
  `spots.py SpaFrankenchamps --puddles`, read out on the answer sheet).
- A dried line felt as the road dries.

**Block F. Front-axle hybrid, Spa, the 919 import (10 min).** Needs P4.
- The pull at both ends on corner exit above its minimum speed; front
  recovery under braking; the ERS HUD showing deploy and harvest.

**Block G. Tread spread, Monza, GT3, toe-in setup (15 min, optional).**
PASTE the P5 line in the garage (also checks setup sharing's PASTE, and
that a line for another car is refused), then a stint of heavy braking.
- The inner/outer readouts (outer and middle hot); a cooked shoulder felt
  as lost grip before the mean leaves the window.
- Lap traces from the pause menu at the end: last against best, braking
  points on the corners, delta's sign.

**Block H. Pad, Sakhir (10 min).** Block D's events replayed with the pad
(the wheel unplugged or its steering unbound).
- Greasy fronts, flat spot, bent suspension, sick engine, gravel and sand on
  the two motors.

**Block I. Imported circuit's pit stop (5 min).** An AC-imported circuit, one
stop by hand: limit, autopilot at the first line, the box, service, hand-back
(H2).

## Phase 2 — agent after each sitting

1. Parse `out/sit/<date>/*/`: every MARK with its 2 s of mixer state, the
   15 s force lines (time at the base's limit, mean torque, correction share),
   and the answer sheet or the saved answers (`verifier.py reviewed`).
2. Classify each row: **OK** → close its item (`verifier.py close`, and
   H1/H2 from [work-plan.md](work-plan.md) once empty); **wrong** → a
   follow-up in [follow-ups.md](follow-ups.md) with the mark's numbers;
   **not felt** → check the log first (did the signal reach the mixer?)
   before calling it a bug.
3. Tune what the numbers explain (the levers named in
   [game/input-and-ffb.md](game/input-and-ffb.md): `WheelTorqueReference`,
   `PNEUMATIC_TRAIL_ZERO_AT_SLIP`, `MaxRimStiffnessPerDeg`, the damper floor,
   `GroundMix` levels). Server-side torque changes go through
   `steering_feel_probe` and the AI survey as every physics change does.
4. Write a **re-sit list**: only the rows the tuning touched, as a short
   block in `sittings.yml`.

## Borrowed hardware

These checks can't be done on the ClubSport V2.5. Each is a 15-minute mini
sitting (block 0 + block A's first three laps) whenever the base is at
hand, and the agent reads the Devices line (name, vid:pid) and the force
lines afterwards.

- A gear-drive base (Logitech G29/G923) and a belt or hybrid one
  (Thrustmaster T300/TS-XW): the 5% / 3% minimum force, centre not notched.
- A direct drive above 8 Nm (Simucube, Moza, Fanatec DD): output scale
  8 / peak, the same Nm in the hands, the soft lock and hit kick not
  dangerous (cap the base's own max strength first).
- A PXN base: the name rules (no vendor id) against what it reports.
- SimHub / rev lights / a motion platform (telemetry output, N2): its own
  sitting once one is set up.

## Effort

| Phase | Who | Time |
|---|---|---|
| 0 prep | agent | ~1 day (P1–P3 are the code; P4–P8 are runs) |
| Sitting 1 | human | ~70 min |
| 2 first pass + tuning | agent | ½–1 day |
| Sitting 2 | human | ~65 min |
| 2 second pass | agent | ½ day |
| Re-sit | human | ~20 min |
