# Follow-ups

What is left on features that are built: bugs, edge cases, rough edges and
the cleanup a feature left behind. What is built but has not been seen,
heard or felt yet is in [verification.md](verification.md); features not
started are in the [roadmap](roadmap.md); simulation and racing-rule gaps
are in [simulation-gaps.md](simulation-gaps.md). An item lives in exactly
one of the four.

Items are grouped into **packages** the same way as the roadmap: one
package is one go (same code, one check, one commit), it says what it is
for, what is in it and how it is checked, and it takes the priority of its
most urgent item. When an item is done, delete its bullet; when a package
has none left, delete the package. Git history keeps what was done.

- **P0**: blocks someone from playing a race start to finish. Fix before anything else.
- **P1**: core sim-racing experience. Players notice at once when it is wrong.
- **P2**: depth and polish.
- **P3**: nice to have.

## P0: playable end to end

None open.

## P1: core racing experience

None open.

## P2: depth

### 2. Road state and snow on screen

Rough edges of the client's road state and snow.

- A puddle covers its cell's width: the server keeps one water depth per
  cell (`RoadState`), so a puddle has no lateral position; placing one
  across the road is a per-bin depth on the wire.

**Check:** a rain-to-dry and a snow session at Silverstone, screenshotted.

### 4. Damage

The car parents, the damage actor and the importer's damage tables.

- The DRS flap and the driver can be dented; wheels are drawn toed.

**Check:** a seeded pileup replay screenshotted before and after; one AC
import re-run and crashed.

### 5. Hotlap and lap records

The ghost, the hotlap replay and watch.

- The TV director's line-of-sight check (`FWorldQueries::IsClear`, a trace
  against world statics) misses buildings with no collision: a trackside
  shot at Norisring stood behind one for seconds in the hotlap replay
  (`GhostTv`), and the broadcast camera would do the same there.
- Corners carry the circuit's official turn number in the dossier, not the
  detector's.

**Check:** a hotlap with ghost and replay; a server restart between two
watches.

### 6. Circuit dressing

Prop-kit and `ats-dress` work on circuits and pieces that are built. Laid by
the same `ApexProps` code.

- Mandarina Bay: corner names (the dossier has one "corner", the circuit
  itself), the F1 pit building (`building_pit_street`) placed, a quay rail
  along the water, a catch fence pass.
- Stadium grandstands: a scene names the stadium bays and cap that
  `LayoutGrandstand` lays; `grandstand/stair_tower` is built.
- Nordschleife: rock cuttings. OSM maps no cliff, bare rock or cutting
  along the lap, and the DEM is a surface model that reads the forest's
  canopy as a slope, so no rule can tell a cutting from a tree line; it
  needs a bare-earth terrain model (Rhineland-Palatinate's DGM1) or a
  surveyed list of the cuttings, then a rock-face kit piece.
- Nordschleife: the lap's bridges over the K 73, the B 257 (Quiddelbach,
  Breidscheid), the L 92 and the L 93 are parapets only
  (`track_bridge_10m`). The ground under the lap is not cut away, so the
  road beneath is not there: it needs an `Underpass` for a dossier road in
  `terrain.rs` (a slot under the deck, as at Suzuka), the road drawn in it,
  and the AI survey.
- Nordschleife: the marshal posts are numbered in lap order from the
  line, not with the real post numbers (OSM maps one, "Posten 44", by the
  GP circuit); a real list goes in a dossier table.
- Nordschleife: the toll lanes stand on the grass: the entrance's access
  lanes and car park are not drawn (no dossier road is baked).
- Camp sites off the fan lap pitch their tents in a row: the walk in
  `dress::surroundings::camp_sites` uses one irrational step for both
  coordinates (0.618 x 1.618 is 1). The fan lap uses the R2 sequence
  (`r2_point`); switching every circuit moves their tents, which are walls
  (`attraction`), so it goes with a re-dress and an AI survey of those
  circuits.
- Nordschleife: the OSM extract the dossier was fitted from is gone from
  `.cache/osm/`, so the surveyed layers cannot be refitted; a fresh
  extract (the map API tiles in `.cache/osm/nordschleife_tiles/` are
  newer than the cut) would change the woods and barriers, and so the
  walls: a refit is a groom and an AI survey of its own.

**Check:** each circuit by hand in the running game; the track-editor tests.

## P3: nice to have

### 9. Car content

Car data that is built but that the client and the shipped cars do not use.

- Shipped car.toml files set their tyre pressure keys and their own
  `[[tires.compound]]` / `[brakes]` tables (all run on defaults today).

**Check:** server physics and class calibration tests (a car.toml edit moves
them, and changes its content checksum).

### 11. Content tooling

- **(bug, not reproduced)** On 2026-10-14, with most showcases stale, an
  unattended run (`-ApexWatch`, `-ApexWatchSession`) exited silently a few
  seconds after "Demo view ended", with nothing in the log and no crash
  report. On 2026-10-15 a `-ApexWatch` run against a server holding only
  stale showcases skipped them, played a local file, restarted on another
  track and kept running; with fresh showcases a 7-minute run was clean.
  If it comes back, the run's `-abslog` and the Windows event log are the
  places to look.
- `dem_elevation.py` handles a circuit that crosses itself (Suzuka: the DEM
  sees only the bridge, so the fit closes the crossover), and Shanghai's and
  Monza's surface models are checked against a bare-earth one (grandstands,
  the park's canopy); all three still drive their invented z.

**Check:** the track-editor tests; a fresh-clone `initialize_content.ps1`;
the launcher import with the network off.

### 12. Rendering

- Runtime-built track meshes get mesh distance fields, or an A/B against an
  `-ImportLevels` level shows they do not need them (software Lumen and DF
  shadows see the road and terrain less well today).

**Check:** the A/B is two screenshots.

### 13. Cleanup

Small and independent. Do one or two alongside whatever package touches
the same files.

- Wire: `SessionKind::Demo` is kept only for old servers and can go once
  showcases are everywhere (today the backdrop still falls back to it
  while the showcases are stale; see package 11).

**Check:** server and client build and tests.
