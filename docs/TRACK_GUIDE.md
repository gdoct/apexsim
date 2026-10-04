# Track guide

A corner-by-corner walk round a circuit, played on the client from files
made offline: no server, no session, nothing streamed. The track picker
opens it (a GUIDE button on each card that has one, "Track guide" in the
detail panel, G / pad Y on the focused card). The guide opens paused over
the start/finish line with a card of the circuit's facts. Next fast-forwards
the recording round the lap to corner 1, which then loops: once at normal
speed, then its middle few seconds in slow motion, again and again, under a
card with the corner's name, speed, gear, braking point and gotchas. The
player steps to the next or previous corner, changes the camera, pauses,
or leaves for the track picker.

## Files

Made by `apexsim-replay guide` (server bin) into `build/guide/` (gitignored,
like `build/showcase`), shipped as `Game/Guide/` beside `ApexSim.exe`:

| File | What |
|---|---|
| `<Stem>.<Class>.guide.json` | the guide: the circuit's facts, every corner, the cameras |
| `<Stem>.<Class>.guide.apxs` | the recording: an ordinary spectator stream (docs/SPECTATOR.md) |

`<Class>` is the car.toml `class` (`F1`, `GT3`, `Hypercar`, `LMP2`). The
client opens the guide of the class of the car picked on the create screen
(`UApexMenuFlowSubsystem`'s pending car), else any class the track has, in
the order F1, Hypercar, LMP2, GT3.

Folders searched, first hit wins: `-ApexGuideDir=<dir>`, `Guide/` beside
`ApexSim.exe` in a package, the repo's `build/guide` in the editor.

## The recording

Three solo runs of the class (different cars of the class where it has
them), each a lone AI car from pole lapping an empty track, so no car is in
another's wake or way. Each run's second lap (its first flying lap) is kept
from a little before its line crossing to a little after the next, and the
three are merged into one stream with car 1 and car 2 following car 0 by
`car_gap_s` each. A run that strikes a lap for track limits is re-run with
another seed: a guide's car does not cut corners.

Car 0 is the guide's car: every speed, gear and braking figure is its own.

## `guide.json`

All times are seconds from the recording's first frame (what
`FApexReplayClip::SampleAt` takes). All positions are the **server frame**
(metres, +X down the start straight, +Y left, +Z up), as `-ApexCamera=`
takes them; `ApexRace::ServerToUnrealPosition` converts.

```jsonc
{
  "version": 1,
  "class": "GT3",
  "display_class": "GT3",            // ApexCatalog::DisplayClass of the class
  "recording": "Spa.GT3.guide.apxs", // beside this file
  "source_crc": 1234567890,          // the track YAML's content_crc (stale check)
  "car": { "id": "uuid", "folder": "posh-gt3rs", "name": "..." },
  "car_gap_s": 2.0,
  "track": {
    "stem": "Spa", "track_id": "uuid", "display_name": "Spa-Frankenchamps",
    "description": "Modelled on ...", "country": "Belgium", "city": "Stavelot",
    "category": "F1", "year_built": 1921,
    "length_m": 7004.0, "altitude_m": 401.0,
    "latitude_deg": 50.43, "longitude_deg": 5.97,
    "elevation_min_m": 0.0, "elevation_max_m": 102.0,  // relative to the start line
    "climb_m": 180.0,                  // total ascent over a lap
    "corners": 19, "left": 9, "right": 10,  // turns (one per change of hand), not stops
    "direction": "clockwise",          // or "anticlockwise"
    "longest_straight_m": 1900.0,
    "drs_zones": 2,
    "lap_time_s": 138.4, "top_speed_kph": 268.0,  // car 0's flying lap
    "notes": ["hand-written overview lines, may be empty"]
  },
  "overview": {
    "time_s": 4.4,                     // the frozen moment: car 0 25 m short of the line
    "cameras": [ /* camera, see below */ ]
  },
  "corners": [
    {
      "number": 1,                     // the guide's stop, 1..N
      "turn_from": 1, "turn_to": 1,    // turns covered, one per change of hand (a chicane is two)
      "name": "Sauce-Source",          // display name, or "Turn 1" / "Turns 8-9"; never a real trademark
      "direction": "right",            // "left" | "right" | "left-right" | "right-left" (a chicane)
      "entry_m": 300.0, "apex_m": 410.0, "exit_m": 480.0,  // centerline stations
      "turn_deg": 160.0,               // total heading change
      "min_radius_m": 25.0,
      "clip": {
        "from_s": 20.1, "to_s": 30.1,  // the loop at normal speed (~10 s)
        "slow_from_s": 23.5, "slow_to_s": 27.5,  // the part shown in slow motion
        "slow_rate": 0.25,
        "apex_s": 25.4                 // car 0 at the apex
      },
      "min_speed_kph": 72.0, "apex_gear": 2,
      "entry_speed_kph": 245.0,        // car 0 as it starts braking (or at entry when flat)
      "exit_speed_kph": 160.0,         // car 0 at exit_m
      "brake_m": 120.0,                // metres before the apex braking starts; null when flat out
      "flat_out": false,
      "elevation_change_m": -8.0,      // exit minus entry
      "banking_deg": 0.0,              // signed: + leans into the corner
      "gotchas": ["generated lines"],
      "notes": ["hand-written lines, may be empty"],
      "cameras": [ /* camera, see below */ ]
    }
  ]
}
```

A **camera** is a fixed tripod or a car-relative view:

```jsonc
{ "name": "Trackside", "kind": "fixed", "eye": [x, y, z], "look": [x, y, z], "fov_deg": 35.0 }
{ "name": "Overhead",  "kind": "fixed", "eye": [...], "look": [...], "fov_deg": 50.0 }
```

Fixed eyes are seated on the ground heightfield offline, but stands, trees
and buildings are not known there: the client traces down (props ignored)
to lift an eye above the ground and keeps a fixed camera only if it can see
its `look` point, else falls back to the next. Car-relative cameras (chase,
onboard, TV locked on car 0) are the client's own and come after the fixed
ones in the cycle; they are not in the file.

## Hand-written notes (`content/tracks/default/<Stem>.guide.yml`)

Checked in beside the YAML; read by `apexsim-replay guide`:

```yaml
overview:
  - "A line of the circuit's character."
corners:
  - at_m: 410          # a station in the corner: the detected corner nearest it (within 150 m) takes the notes
    name: "Sauce-Source" # optional: overrides the display name (keep it a sound-alike, never the real name)
    notes:
      - "Brake in a straight line; the hairpin tightens on the way in."
```

Real corner names are trademarks often enough that the rule of
CLAUDE.md "No trademarks on screen" applies to every string here.
