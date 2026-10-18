# Wheel sitting plan

Every check that needs hands on a wheel, done in as few sittings as possible.
It covers work-plan item H1, the wheel half of H2 and the wheel rows of
[verification.md](verification.md) §2. [verification.md](verification.md)
stays the source of truth for *what* is unverified. This file says *how* to
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

## Phase 0 — agent prep (no human)

Done and committed before the sitting is booked. All 🟢.

| # | Prep | Why |
|---|---|---|
| P1 | **Sitting runner**: `scripts/verify/run_checks.py --sit <block>` (or `human: true` on a run) starts its own server with the block's `server.events` and env, launches the game with the block's `args` and no stand-in, takes no shots, waits until the game is closed, then copies the game log, `Saved/Telemetry` and the server log into `out/sit/<date>/<block>/`. Blocks live in `scripts/verify/sittings.yml` in the `runs.yml` format. | One command per block, nothing to type at the rig. |
| P2 | **MARK**: a wheel-column slot `Mark` (appended to `ApexInput::Slots()`, unbound by default) plus `apexsim.debug.Mark <word>`. A press writes a `MARK` log line: session time, track station, speed, gear, steer input, rim degrees, the last 2 s of the wheel mixer's ring buffer (torque in, constant out, correction, road, vibration source) and the newest `DriverFeedback`. A short cue confirms it. | A finding becomes "mark 3 = too light", with the numbers already logged. |
| P3 | **Tyre heat event**: `heat=FL:C` (or `heat=front:C`) in the server's debug events, so "greasy fronts" can be set at a known time rather than cooked by hand. `tests/debug_hooks_test.rs`. | Block D sets cooked fronts in seconds, not ten laps. |
| P4 | **Front-axle hybrid car**: re-import the AC 919 with `[FRONT_MOTORS]` (`scripts/ac_car_import.py`, the AC install under `E:\SteamLibrary`) and check its `[hybrid] axle = "front"`; server and client car tests green. | Block F has a car to drive. |
| P5 | **Toe-in setup as a text line** (the garage's COPY AS TEXT format) with a lot of front toe-in, stored in `sittings.yml`. Block G pastes it with PASTE. | The setup comes ready, and the PASTE row of setup sharing gets checked along the way. |
| P6 | **Answer sheet**: `out/sit/<date>/sheet.md` generated from `sittings.yml`, one row per check with its answer words, ready to fill in during the sitting (a second screen, or read aloud and typed afterwards). | No free-form write-up. |
| P7 | **Builds**: an editor build with the physics library (`play_editor.ps1 -Build`) and a server in a scratch `CARGO_TARGET_DIR`. Automation `ApexSim.Input.*`, `ApexSim.Prediction.Feel.*` green. | Nothing is built while the human waits. |
| P8 | **Pre-flight probe** (no forces): `apexsim.input.Devices` lists COL01/COL02 with their axes, the profile resolves to ClubSport V2.5 at scale 1, and `ApexProfile.sav` is backed up (the runner overwrites it). | Problems with device detection or profiles show up before anyone sits down. |

The human's only prep: the Fanatec App shows no pending firmware (a driver
update without firmware silently drops game forces; see
[game/input-and-ffb.md](game/input-and-ffb.md#traps)). Write down the base's
FF / NDP / NFR / SEN menu values once at the start.

## Phase 1 — the sitting

About 2 h 15 min of driving, best split into **Sitting 1** (blocks 0–C,
core feel, ~70 min) and **Sitting 2** (blocks D–I, conditions and extras,
~65 min), with the agent's first pass in between: if Sitting 1 finds the
base feel wrong, Sitting 2's conditions would be judged on a wrong baseline.

Every block: the agent launches it, the human drives, presses MARK with a
word, then fills in the block's rows. Every block's `Wheel forces over 15 s
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
  `spots.py Spa --puddles`, read out on the answer sheet).
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
   and the answer sheet.
2. Classify each row: **OK** → delete it from [verification.md](verification.md)
   (and H1/H2 from [work-plan.md](work-plan.md) once empty); **wrong** → a
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
