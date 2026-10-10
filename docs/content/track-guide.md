# Track guide

A corner-by-corner walk round a circuit, opened from the track picker and
played entirely on the client from two files made offline, per circuit and
car class. Nothing touches the server at runtime. The guide opens paused over
the start line with a card of the circuit's facts; Next fast-forwards to
corner 1, which then loops (once at normal speed, then its middle in slow
motion) under a card with the corner's name, speed, gear, braking point and
gotchas. The player steps between corners, changes camera, pauses or leaves.

## Code

- `server/src/track_guide.rs`: corner detection, the solo runs, measuring,
  gotchas, cameras, the guide file (`TrackGuide`), and `wire_corners` (the
  same corners sent as `TrackCorners` to a watched hotlap, see
  [sessions](../server/sessions.md)).
- `server/src/bin/apexsim_replay.rs`: the `guide` subcommand.
- `content/tracks/default/<Stem>/<Stem>.guide.yml`: hand-written notes,
  checked in.
- Client: `game-unreal/Source/ApexSim/Public/Guide/` (`UApexTrackGuideSubsystem`,
  `ApexGuide::FPlayer` in `ApexGuidePlayer.h`), `UI/ApexTrackGuideWidget`.
- Pipeline: `scripts/lib/ApexGuide.ps1`.

## Making guides

```bash
apexsim-replay guide --all                     # every shipped circuit x every class (a few minutes)
apexsim-replay guide content/tracks/default/Spa/Spa.yaml --class GT3
apexsim-replay guide --all --report            # detected corners, to author notes against (no sim)
apexsim-replay guide --all --missing-only      # only guides older than their YAML, notes, dossier or car.tomls
```

Output goes to `build/guide/` (gitignored): `<Stem>.<Class>.guide.json` and
its recording `<Stem>.<Class>.guide.apxs`, an ordinary
[spectator stream](../game/spectator.md). `<Class>` is the car.toml `class`.
`initialize_content.ps1` builds what is missing or stale (stage 8),
`build_game_standalone.ps1` copies `build/guide` to `Guide\` beside
`ApexSim.exe` (`-SkipGuide`), `build_release.ps1` builds stale guides and
ships them in `Game\Guide`.

### The recording

Three solo runs (`solo_run`): one AI car alone from pole on an empty track, at
skill 110, sunny 13:00 with no wind (`guide_conditions`). Car 0 is the
class's first car by folder and is the guide car: every figure on the cards is
its own; cars 1 and 2 are the next cars of the class. Each run's first flying
lap is kept and the three are merged into one stream `--gap` (2 s) apart, so
a corner's loop shows three clean passes with nobody in anyone's wake.

- A run that leaves the track or strikes its lap is tried again (`best_run`,
  up to `--tries`, 6) with the next seed and two points less skill (not under
  90): at the top of the scale the AI has no randomness, so another seed alone
  would be the same lap. The least bad run is kept if none is clean.
- A car that cannot make a flying lap at all hands its place to the next car
  of the class.
- The guide cars set `AiDriverProfile::exact_line`, which lifts the
  `MAX_LINE_PRECISION` cap every racing AI keeps off the edge, so they drive
  the raceline itself; the tool prints each run's RMS distance from the
  raceline and the centerline. A track with no `raceline` is driven on its
  centerline (by every race too): `scripts/generate_race_line.py --track
  <yaml>` writes a minimum-curvature line into the YAML; re-export the track
  after.

### Corners (`detect_corners`)

Curvature every 2 m, smoothed over 24 m. A run tighter than 450 m radius for
30 m and 15 degrees is a turn; same-hand runs within 30 m merge (a double
apex); runs within 30 m of each other group into one guide stop of up to
three (a chicane, esses) while the stop spans under 300 m. A stop's
`turn_from` / `turn_to` count one turn per change of hand. Names
(`name_corners`) are the dossier's `display_name` of the way nearest the apex,
never its real `name`, each used once, names of straights skipped; else
"Turn N" (see [naming](naming.md)).

### Measured (`measure`, `gotchas`)

From the guide car's lap: the slowest point and its gear; the braking point
(the brake run that sheds the most speed between the previous apex and this
one, gaps under 0.33 s bridged; throttle is no test, since the AI's throttle
on a straight is about 0.7); flat out / lift / braked; entry and exit speed;
the clip windows (normal speed from about 1.5 s before braking until the last
car is 2.5 s past the apex; slow motion at 0.25 from turn-in to exit, at most
4.5 s). Gotchas (at most four, ranked) read the geometry: big stops, uphill or
downhill braking, crests and compressions at the car's speed, banking,
chicanes, hairpins, tightening or opening radius, a wall close on the exit
(`walls.msgpack`), tarmac run-off (`curbs.msgpack`), a long straight or a DRS
detection after. Fixed cameras (trackside outside the apex, braking zone, exit
looking back, overhead) are seated on the ground sidecar.

The figures are the sim's, not the real circuits': the AI's laps run slower
than real lap times.

## `guide.json`

Times are seconds from the recording's first frame (what
`FApexReplayClip::SampleAt` takes); positions are the **server frame**
(metres, +X down the start straight, +Y left, +Z up), as `-ApexCamera=` takes
them.

```jsonc
{
  "version": 1, "class": "GT3", "display_class": "GT3",
  "recording": "Spa.GT3.guide.apxs",   // beside this file
  "source_crc": 1234567890,            // the track YAML's content checksum
  "car": { "id": "uuid", "folder": "posh-gt3rs", "name": "..." },
  "car_gap_s": 2.0,
  "track": {
    "stem": "Spa", "track_id": "uuid", "display_name": "Spa-Frankenchamps",
    "description": "...", "country": "...", "city": "...", "category": "F1", "year_built": 1921,
    "length_m": 7004.0, "altitude_m": 401.0, "latitude_deg": 50.43, "longitude_deg": 5.97,
    "elevation_min_m": 0.0, "elevation_max_m": 102.0,   // relative to the start line
    "climb_m": 180.0,                                   // total ascent over a lap
    "corners": 19, "left": 9, "right": 10,              // turns, not stops
    "direction": "clockwise", "longest_straight_m": 1900.0, "drs_zones": 2,
    "lap_time_s": 138.4, "top_speed_kph": 268.0,        // the guide car's flying lap
    "notes": []
  },
  "overview": { "time_s": 4.4, "cameras": [] },         // the guide car 25 m short of the line
  "corners": [{
    "number": 1, "turn_from": 1, "turn_to": 1,
    "name": "Sauce-Source",            // display name, or "Turn 1" / "Turns 8-9"
    "direction": "right",              // "left" | "right" | "left-right" | "right-left"
    "entry_m": 300.0, "apex_m": 410.0, "exit_m": 480.0,
    "turn_deg": 160.0, "min_radius_m": 25.0,
    "clip": { "from_s": 20.1, "to_s": 30.1, "slow_from_s": 23.5, "slow_to_s": 27.5,
              "slow_rate": 0.25, "apex_s": 25.4 },
    "min_speed_kph": 72.0, "apex_gear": 2, "entry_speed_kph": 245.0, "exit_speed_kph": 160.0,
    "brake_m": 120.0,                  // metres before the apex; null when flat out
    "flat_out": false, "elevation_change_m": -8.0,
    "banking_deg": 0.0,                // signed: + leans into the corner
    "gotchas": [], "notes": [],
    "cameras": [{ "name": "Trackside", "kind": "fixed", "eye": [0, 0, 0], "look": [0, 0, 0], "fov_deg": 35.0 }]
  }]
}
```

## Hand-written notes (`<Stem>.guide.yml`)

```yaml
overview:
  - "A line of the circuit's character."
corners:
  - at_m: 410            # the detected stop nearest this station (within 150 m) takes the notes
    name: "Sauce-Source" # optional: overrides the name; keep it a sound-alike, never the real one
    notes:
      - "Brake in a straight line; the hairpin tightens on the way in."
```

## The client

`UApexTrackGuideSubsystem` finds `<Stem>.<Class>.guide.json` in
`-ApexGuideDir=`, `Guide/` beside the exe, or the repo's `build/guide` (first
hit wins; a guide counts only when the track's export is installed). It picks
the class of the create flow's pending car, else F1, Hypercar, LMP2, GT3 in
that order, loads the `.apxs` as an `FApexReplayClip` and drives the race
director's replay view with its own clock (`BeginGuideView`,
`SetGuideClock(Seconds, Rate, bCut)`, `SetGuideCamera`, ended by
`EndReplayView`; engines are muted off 1x).

`ApexGuide::FPlayer` is the pure state machine: Overview (frozen), Travel
(6x, a far corner jump-cut to 24 s short of it), Normal, Slow; every backwards
move and loop seam is a cut. The camera cycle is the file's tripods (kept only
if a down-trace can seat them and they can see their look point, since stands
and trees are unknown offline; they pan toward car 0), then Broadcast (TV
locked on car 0), Chase, Onboard.

The track picker shows a GUIDE chip on each card that has one
(`UApexContentCardWidget`'s secondary action) and a "Track guide" button (G /
pad Y). While open the guide layer owns the keys: arrows / PageUp/Down /
shoulders / D-pad step corners, C or pad Y next camera, Shift+C or pad X the
previous, Space / pad A pause, Home the overview, 1-0 a corner, Esc /
Backspace / pad B back to the picker.

Console: `apexsim.guide.Open <Stem> [Class]`, `apexsim.guide.Next`,
`apexsim.guide.Prev`, `apexsim.guide.Corner N`, `apexsim.guide.Camera [N]`,
`apexsim.guide.Pause`, `apexsim.guide.Close`, `apexsim.guide.Rescan`.
Unattended: `-ApexGuide=<Stem>[:Class] -ApexGuideCorner=N -ApexGuideCamera=N`
(`-ApexScreenshotAfter` works outside a race).

## Checking it

- `cargo test track_guide` (`track_guide::tests`: stops, names, notes, the
  turn count at Zandvoort).
- `ApexSim.Guide.*` (parser, file index, player, keys, text).
- `apexsim-replay guide <yaml> --report` to see the detected corners.

## Traps

- Every string a guide shows must be a display name: corner names come from
  `display_name`, and a `.guide.yml` `name` override must not be the real one.
- A guide is stale when its YAML, notes, dossier or a car.toml of the class
  changes (`--missing-only` checks file times); rebuild after any of them.
- Retrying a solo run with a new seed alone does nothing at skill 110; the
  retry must change the skill.
