# Correcting the dossiers from Assetto Corsa: `ac_layout.py`

*2026-10-02. Twelve native circuits have an Assetto Corsa counterpart on
the development machine. The native road (a GPS trace with real elevation)
stays as it is. AC places the stands, buildings, pit lane, bridges, masts
and trees better than OpenStreetMap does, so this tool reads those
positions off the AC track and writes them into the circuit's dossier
(`<Stem>.layout.json`). `ats-dress` then lays kit props there.*

```powershell
python scripts/ac_layout.py --pairs                  # the pairs and whether their AC folders exist
python scripts/ac_layout.py Zandvoort --dry-run      # report + overlay.png, writes nothing in content/
python scripts/ac_layout.py Zandvoort                # write the overlay and apply it to the dossier
python scripts/ac_layout.py --all [--dry-run]
python scripts/ac_layout.py Zandvoort --unapply      # back to the OSM dossier, byte for byte
#   --ac-root <AC content/tracks> (or APEXSIM_AC_TRACKS), --no-cache, --force
python -m unittest scripts/ac_import/tests/test_ac_layout.py
```

## What is kept, and why that is allowed

Only measurements are stored: a footprint box (centre, yaw, length, depth),
a stand's road-facing front, a height class, the pit lane polyline and its
garage count, a crossing's station, a landmark's point and kind, and wood
outlines. No mesh, texture or kn5 name is kept, and every prop is still
drawn from the kit. This is the "kn5 as a survey" category from
`AC_IMPORT_FEASIBILITY.md`: facts about where things stand at a real
place, like the OSM dossier. `AC_TRACK_IMPORT.md` keeps its rule that no
AC geometry, textures or physics are distributed.

Every dossier's `attribution` names the AC source, and the pairs file
records its origin. Four of the twelve sources are mods: Interlagos,
Suzuka 0.9, Le Mans 2017, and the `monza` folder, which a 2022 mod has
overwritten. If only Kunos-derived corrections should ship, delete those
overlays and run `--unapply` on them.

## Pipeline

1. **Pairs.** `content/tracks/ac_pairs.json` maps each native stem to an AC
   folder and layout. It also records the origin and, optionally,
   `"pit": "auto" | "ac" | "native"`.
2. **Survey** (`scripts/ac_import/features.py`). The importer's own readers,
   frame and physics world run on the layout's kn5s. Its scenery selection
   is reused: renderable, LOD 0, not logic, not overlays, not invisible.
   - **Meshes become pieces.** Each mesh is split into connected pieces, with
     vertices welded at 1 cm.
   - **Left out:**
     - flat pieces at ground level: relief under 1.5 m and not lifted 2.5 m
       off the ground;
     - terrain sheets: over 5 ha, or ground or terrain names such as
       `mount`, `rock` or `stones`;
     - barriers (the groom lays our own);
     - clutter: parked cars, vehicles, tents, toilets, kerbs, marshal posts.
   - **Tree cards** (`ksTree`, or a masked material named like a tree)
     become points, with the leaf type taken from the names.
   - **Seated spectator cards** count as evidence for a stand. They are
     never merged into geometry: merged, a crowd spread over a bank once
     stitched trucks, tents and trees into one object. Standing spectators
     (`ppl-stand`) are ignored.
   - **Merging pieces into objects.** Pieces whose outlines touch are
     merged into one object. If the pieces cover under 45% of the result's
     hull, it is a loose cluster, and it is regrouped using only real
     overlaps. Without this, Spa's car parks and the main grandstand next
     to them became one 340 m "building".
   - **Classification:**
     - **crossing:** an object with the road under it and its underside
       4 m or more above the road. It is a footbridge when the deck is up
       to 6 m wide. Crossings within 40 m of the start line are skipped;
       the start gantry is built by the bake.
     - **landmark:** a slender mast with a light name (floodlight), or a
       wheel, screen or camera tower by name.
     - **pit complex:** an object beside the pit lane. It is never emitted,
       because the bake builds the garages from the lane.
     - **stand:** a stand by name (`tribun`, `gstand`, `seduta`…), or 20 or
       more seated cards inside it, or a **tiered profile**: across the
       object's depth, away from the road, the upper quartile of its
       vertex heights climbs by 4 m or more with a correlation of 0.7 or
       higher. Kunos name the Red Bull Ring's stands `concrete` and
       `Metal_new`.
     - **structure:** anything 2.5 m tall and 250 m² or larger.
   - **Pit lane.** AC's `pit_lane.ai` is the AI's whole route through the
     pits, including the race track before and after. It is trimmed to the
     longest stretch that is off the road, plus the node at each end.
     `AC_PIT_n` gives the garage count.
3. **Fit** (`ac_layout.Fit`).
   - **Rigid fit.** The AC centerline is fitted onto the native one with
     the dossier's own FFT search and trimmed ICP (`osm_layout`). The two
     frames are far apart: rotated by up to 166 degrees, with start lines up
     to 300 m apart.
   - **Rubber sheet.** The rigid fit leaves stretches where one trace is
     bent against the other. Locally the shapes still agree, so these are
     shifts, not different roads:

     | Circuit | Shift | Stretch |
     |---|---|---|
     | Zandvoort | 10–16 m | 1.8–2.1 km |
     | Interlagos | 7–11 m | 0.9–1.35 km |
     | Le Mans | 20 m | one spot |

     Every AC point is therefore moved by the native road's displacement
     beside it: a Gaussian blend of the centerline's displacements, with
     sigma 12 m on the road growing to 60 m from 120 m out, fading to
     nothing between 150 and 300 m. This takes the share of the lap within
     3 m from 51–95% (rigid) to 95–100%.
   - **Gates:**
     - the rigid fit has 40% of the lap within 3 m;
     - the field has 90% within 3 m;
     - scale is within 0.5%;
     - under 15% of the lap is *reshaped*, meaning the roads still disagree
       by more than 3 m after the field.

     Features within 50 m of a reshaped stretch are held for review. None
     of the twelve has one longer than 60 m.
4. **Entries.** The survey's objects become dossier entries in the native
   frame, anchored by `osm_layout.Track`'s own helpers:
   - range filters are the dossier's: stands 260 m, buildings 140 m,
     landmarks 300 m, woods 260 m;
   - a footprint that would stand on the native road is held for review;
   - woods are tree cells of 10 m with at least 2 trees, closed by two
     cells, outlined and simplified to 6 m;
   - the pit lane's ends are re-joined to the native road edge 40 m beyond
     where they stop short.
5. **Merge** (`ac_layout.merge`, also run by `osm_layout.py` after every
   refetch):
   - **Matches.** An AC footprint that covers 30% of the smaller of itself
     and a dossier entry is a match. It takes AC's geometry and keeps the
     dossier's name. If OSM has the entry as a grandstand, it stays a stand
     even when AC calls it a building.
   - **AC-only** entries are added.
   - **Dossier-only** entries stay, because the AC version may be older.
     They are removed only if they collide with an AC entry (10% overlap).
   - **Manual entries are never replaced:** `MANUAL_STRUCTURES`,
     seating-map stands, authored crossings and landmarks.
   - **Footprints that differ in size by more than 1:5 are not a match.**
     This leaves the dossier's entries in place, as at Suzuka's hairpin
     paddock, which came out as one 130 m square.
   - **Crossings and landmarks** move to AC's station or point, keeping
     the dossier's name, kind and brand.
   - **Woods** are a union of both.
   - **The pit lane.** AC's lane replaces the dossier's only if it reaches
     the dossier's entry and exit within 100 m. AC's pit spline often
     rejoins the track before the real exit lane ends: Interlagos' is
     640 m against OSM's 1380 m. Otherwise the dossier's lane stays and
     gets AC's garage count as `box_count` (`dress::build_pit_lane` caps
     it by what the lane holds). `MANUAL_PIT_LANE` always wins.
6. **Persistence:**
   - `<Stem>.layout.ac.json` is checked in. It holds the fit, the entries,
     the review list, the AC files' CRCs and the CRC of the native
     centerline's plan (node x and y to the centimetre).
   - `osm_layout.py` re-applies the overlay after a refetch. If the road's
     plan has moved since the fit, it writes the dossier without the
     overlay and warns that `ac_layout.py` needs to be re-run. Elevation,
     banking, DRS and metadata rewrites leave the overlay valid.
   - The merged dossier carries an `ac_survey` block: the dossier entries
     it replaced and where they were, and the pit lane before. That is
     what `--unapply` uses, byte for byte.
   - `build/ac_layout/<Stem>/report.json` and `overlay.png` are the review
     material. On the image: grey is the native road, blue the fitted AC
     line, red the dossier before, green from AC, and yellow held for
     review.

## Results (2026-10-02, all twelve applied)

| Circuit | Within 3 m: rigid → field | Shift: median / max (m) | AC stands / buildings | Matched (median move) | Added | Crossings / landmarks / woods | Pit lane |
|---|---|---|---|---|---|---|---|
| Zandvoort | 51% → 98% | 2.7 / 16.5 | 1 / 2 | 3 (3.2 m) | 0 | 2 / 0 / 4 | dossier's + 18 garages |
| Silverstone | 88% → 95% | 0.7 / 8.7 | 29 / 26 | 34 (8.8 m) | 18 | 4 / 16 / 15 | AC's |
| Spielberg | 88% → 99% | 1.0 / 4.4 | 1 / 10 | 9 (13.1 m) | 2 | 4 / 0 / 17 | dossier's + garages |
| Spa | 95% → 99% | 0.4 / 6.8 | 4 / 7 | 4 (4.5 m) | 7 | 0 / 0 / 53 | AC's |
| Monza | 87% → 98% | 0.6 / 6.7 | 16 / 12 | 13 (10.8 m) | 12 | 1 / 0 / 27 | dossier's + garages |
| Suzuka | 88% → 98% | 1.1 / 9.5 | 17 / 18 | 16 (7.6 m) | 13 | 1 / 4 / 40 | dossier's + garages |
| SaoPaulo | 58% → 99% | 1.7 / 11.8 | 9 / 25 | 10 (8.3 m) | 24 | 1 / 2 / 18 | dossier's + garages |
| LeMans | 82% → 98% | 0.8 / 20.0 | 27 / 35 | 36 (10.2 m) | 23 | 2 / 6 / 97 | AC's |
| BrandsHatch | 94% → 99% | 0.6 / 6.6 | 3 / 19 | 9 (3.9 m) | 12 | 0 / 0 / 15 | AC's |
| Nuerburgring | 75% → 96% | 0.9 / 16.2 | 22 / 11 | 12 (26.8 m) | 11 | 1 / 0 / 31 | dossier's + garages |
| Nordschleife | 80% → 100% | 1.3 / 7.8 | 2 / 34 | 14 (16.3 m) | 20 | 3 / 0 / 147 | AC's (unused: no pit lane there) |
| Catalunya | 95% → 99% | 0.5 / 4.4 | 19 / 9 | 19 (6.2 m) | 8 | 2 / 0 / 29 | dossier's + garages |

`ats-dress --dry-run` on all twelve lays every circuit. Stands go from 168
to 236 and buildings from 511 to 689. The entries that cannot be laid are
the same kind as before: pit-complex buildings, plus one Brands Hatch
building that cannot clear the road.

**Wall openings** (`check_walls.py --openings`, metres of lap edge with no
wall within 45 m): 310 m before, about 600 m after.

| Circuit | Before → after (m) | Cause |
|---|---|---|
| Silverstone | 28 → 238 | 148 m of it is the pit-entry apron along AC's longer entry lane, which is narrower than `PIT_TAPER_WALL_MIN_M`, so the bake walls none of it |
| Brands Hatch | 2 → 60 | many 2–8 m gaps where the barrier line stops at a new building and the building's face does not close it |
| Suzuka | 78 → 94 | same gaps at new buildings |
| Nürburgring | 38 → 50 | same gaps at new buildings |

Le Mans fell from 40 to 20. The others are within a few metres. Run the AI
survey on these circuits before shipping them.

## Known limits

- **Older AC versions.** Kunos Zandvoort predates the 2020 rebuild, so it
  adds almost nothing there, and its pit lane is the old one (kept out by
  the span rule).
- **Kit fidelity.** A building's footprint is its convex hull, so an
  L-shaped building is a box. The kit has four building types, chosen by
  size and levels.
- **Bridges.** AC's overhead advertising and light gantries come out as
  footbridges (Zandvoort's two, Spielberg's three).
- **Woods** are blocky at 10 m cells, simplified to 6 m.
- **Seating test.** Roofed stands whose roof covers the whole depth are
  found only by name, by seated crowd cards, or by matching an OSM
  grandstand.
