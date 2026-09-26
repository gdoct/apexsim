# Bringing the remaining 17 circuits up to the dossier standard

Five circuits (Spa, Monza, Le Mans, Zandvoort, Silverstone) were rebuilt from
public data in September 2026: their scenery now comes from an OpenStreetMap
**layout dossier** (`content/tracks/real/<Stem>.layout.json`) applied by
`ats-dress`, instead of the old procedural enrichment pass that guessed
everything beside the road. This document is the work order for doing the
same to the other 17. Each track is one self-contained task; the per-track
cards in §6 are meant to be handed to one agent each.

Read `CLAUDE.md` §"Real-world layouts" and §"Track pipeline into Unreal"
first. The module docs in `scripts/osm_layout.py`, `track-editor/core/src/dress.rs`
and `track-editor/core/src/bin/ats-dress.rs` are the authoritative description of
what the tools do; this document only says what to do with them.

## 1. Definition of done (per track)

A track is "refined" when all of the following hold. Do not report a track
finished with any of them missing; say which ones are missing and why.

1. `scripts/osm_layout.py` has a `BBOXES` entry for the stem, and
   `python scripts/osm_layout.py <Stem>` writes the dossier without being
   refused (`fit.centerline_covered >= 0.9`, `fit.rmse_m <= 2.5`; Le Mans is
   the only sanctioned exception to the coverage rule, see §5).
2. The dossier's `pit_lane` is present and on the **real** side of the
   track, in course order (entry first). Check against satellite imagery or
   a circuit map; the old scenes had Zandvoort's on the wrong side.
3. Every grandstand on the circuit's published seating/spectator map is in
   the dossier, named as the circuit names it, on the correct side, either
   traced from OSM (`source: "osm"`) or added as a `MANUAL_STANDS` entry
   (`source: "seating map"`). Temporary stands that exist only on a race
   weekend count if they are on the published map.
4. Structures that span the road (footbridges, road bridges, a hotel or a
   pit-building wing over the track) are in `crossings`, not laid as
   buildings across the track.
5. Landmarks that define the venue (a tower, a wheel, floodlight masts for a
   night race) are in `landmarks` with a kind `dress.rs` knows (§3.4).
6. `woods` reflect the real woodland; a desert or street circuit has few or
   none, and that is correct. Do not invent trees.
7. `ats-dress content/tracks/real/<Stem>.yaml` runs clean: the report lists
   the placed counts and `--verbose` shows a `skipped` list you have read
   and can explain (a commentary box "too small to be seating" is fine; a
   named main grandstand skipped is not).
8. Running `ats-dress` a second time changes nothing (`git status` shows the
   `.ats` unchanged after the second run).
9. `ats-export` regenerates the sidecars, the wall sanity scan (§4.3) passes,
   and `cargo test` in `track-editor` passes.
10. The track bakes (`scripts/build_track_levels.ps1 -Track <Stem>`), the game
    builds it at runtime, and at
    least two screenshots were taken and looked at: the pit straight from the
    grid, and the circuit's signature corner from the outside. Nothing stands
    on the road, the pit building is on the pit side, stands face the track.
11. The manual entries carry a comment naming their source (the seating map
    URL, the OSM feature, or "authored: reason"), as the existing Spa,
    Zandvoort and Le Mans entries do.

## 2. The pipeline, step by step

All commands run from the repo root. `ats-export` resolves paths relative to
the working directory, so never run it from `track-editor/`.

### 2.1 Fetch and fit

```bash
# 1. add the stem to BBOXES in scripts/osm_layout.py (seed values in §6)
python scripts/osm_layout.py <Stem> --dry-run      # fetches, fits, reports; writes nothing
python scripts/osm_layout.py <Stem>                # writes content/tracks/real/<Stem>.layout.json
python scripts/osm_layout.py <Stem> --offline      # re-run against the cached extract
```

The console lines to read are

```
   fit: rmse 1.79 m, centerline covered 100.0%, raceway matched 55.1% (12s)
   11 named corners, 42 stands, 21 structures, 3 crossings, 4 landmarks, 9 woods, pit lane right
```

`centerline covered` is the share of the YAML centerline that an OSM
`highway=raceway` way explains to within a few metres; it is the acceptance
gate. `raceway matched` is the reverse (how much of the OSM raceway the
centerline explains) and is low whenever the bbox holds other layouts (an
oval, a Nordschleife, a support circuit). Low `raceway matched` is fine; low
`centerline covered` is not.

Raw extracts cache under `content/tracks/osm-cache/<Stem>.<tile>.json`
(gitignored). The OSM map API refuses a bbox with more than 50k nodes; if the
fetch fails, split the bbox into tiles as `LeMans` does. Overlapping tiles are
fine; ways are deduplicated by id.

### 2.2 Review the dossier

Open the JSON and check, in this order:

- `pit_lane.side` and `pit_lane.nodes[0]` (the entry). Compare with satellite
  imagery. If the script picked the wrong lane (a support-paddock lane, a
  service road), the candidate filter in `extract()` is what to look at: the
  chosen lane is the longest chain within 70 m of the track that reaches the
  start/finish line.
- `corners`: named raceway sections from OSM. Zero named corners means the
  OSM raceway ways carry no `name` tags; that is acceptable but note it.
- `stands`: every entry has a `name`, `side`, `length_m`, `depth_m`, `covered`
  and a `front` polyline. Compare the list against the circuit's seating map.
  Missing stands become `MANUAL_STANDS` entries (§3.2). A stand with
  `length_m < 12` is dropped by dress as a commentary box; that is intended.
- `structures`: buildings within 140 m and over 250 m². Anything that is
  really a bridge should be absent (the script drops `building=bridge`); a
  large building that OSM tagged as something exotic may need a manual
  crossing entry instead.
- `crossings`: bridges over the road with `kind` `footbridge` or `road`. A
  hotel or pit-building wing over the track is neither; author it (§3.3).
- `landmarks`: big wheels, towers and floodlights OSM has. Add what it
  lacks (§3.4).
- `woods`: rings with `leaf` `broadleaved` / `needleleaved` / `mixed`.

### 2.3 Dress and groom

```bash
cargo run --manifest-path track-editor/Cargo.toml --bin ats-dress -- content/tracks/real/<Stem>.yaml --verbose
cargo run --manifest-path track-editor/Cargo.toml --bin ats-dress -- content/tracks/real/<Stem>.yaml --dry-run   # preview only
```

`ats-dress` deletes and re-lays every grandstand, building, attraction,
bridge, light, vehicle and sky prop and the pit lane, then grooms (barriers,
tire walls, boards, trees) around them with tree belts confined to the
dossier's woods. Read the report:

- `stands N (M props)`, `buildings`, `bridges`, `landmarks`, `pit lane`:
  compare with the dossier counts. A big gap means a placement rule rejected
  entries; `--verbose` lists each with its reason.
- Reasons you fix in the dossier/manual tables: "nothing to build clear of
  the pit lane" on a main stand that is really behind the pit building (the
  bake generates the garages, so the terrace over them is correct to drop;
  a separate stand across the track is not), "reaches across the road" /
  "runs across the road" on a building (it is probably a bridge: move it to
  `MANUAL_CROSSINGS`), "cannot be laid clear of the road" (the OSM outline is
  wrong for our purposes or the building is a pit structure).
- Run it a second time. The `.ats` must not change (both passes recycle ids
  and use no RNG or wall clock). If it does, something is non-deterministic
  and that is a bug to report, not to work around.

Do not hand-edit the `.ats` for anything a dossier field can express; the
next `ats-dress` run would throw the edit away.

### 2.4 Export, bake, look

```bash
cargo run --manifest-path track-editor/Cargo.toml --bin ats-export -- content/tracks/real/<Stem>.yaml
cd track-editor && cargo test && cd ..
./scripts/build_track_levels.ps1 -Track <Stem>          # dress, export, preview for one circuit (built by the game at runtime)
```

The export writes `<Stem>.ground.msgpack`, `<Stem>.curbs.msgpack` and
`<Stem>.walls.msgpack` beside the YAML (gitignored; the server reads them at
load) and `content/tracks/export/<Stem>.uescene.json`. Then take screenshots
with a running server (build the server into a scratch `CARGO_TARGET_DIR` if
the user's own server is running and locks the exe):

```
UnrealEditor.exe game-unreal/ApexSim.uproject -game -windowed -resx=1920 -resy=1080
    -ApexAutoRace -ApexTrack=<lobby name substring> -ApexAiCount=4 -ApexScreenshotAfter=25
    [-ApexCameraLookAt=X,Y,Z,TX,TY,TZ]     # server-frame metres: pick a pose from the YAML centerline
```

Screenshots land in `game-unreal/Saved/Screenshots/WindowsEditor/`; the game
does not exit by itself. `scripts/play_editor.ps1` starts a local server for
you when nothing is listening.

### 2.5 Commit

Commit per track: the `osm_layout.py` table additions, the new
`<Stem>.layout.json`, the rewritten `<Stem>.ats`. The msgpack sidecars, the
OSM cache and the export JSON are gitignored and stay that way.

## 3. Authoring rules for the manual tables

Everything OSM does not carry lives in three dicts in `scripts/osm_layout.py`
keyed by stem, and survives a refetch. All distances are metres; all
stations are measured along the YAML centerline from the start/finish line
in race direction, wrapping at the lap length. The script prints nothing
about stations, so find them by locating a landmark's coordinates in the
YAML `nodes` (the file is in the server frame: origin at the start line,
+X along the track at the line, +Y to the left) or by reading `station_m`
of a nearby OSM-traced entry in a first dossier and adjusting.

### 3.1 Sides

`side` is `"left"` / `"right"` **as seen driving the lap**, or `"outside"` /
`"inside"` of the bend across the span (resolved from the centerline's own
curvature, so use those for stands at corners: that is how spectator maps
describe them).

### 3.2 `MANUAL_STANDS`

```python
dict(name="Tribune Pouhon", from_m=3890, to_m=4060, side="outside", depth_m=14)
# optional: gap_m (road edge to front row, default 10), covered=True/False (default False)
```

The front is laid along the road edge over the span, so it curves with the
bend. `depth_m >= 13` selects the tall two-tier `_large` family, `covered`
adds a roof; `depth_m >= 25` is always roofed. Name stands as the circuit
does (local language is fine). A stand within 20 m of the pit lane's centre
is dropped by dress (the bake builds the pit terrace from the lane), so do
not add the main stand *over the garages*; add the one *across the track*
from them, which is where the main grandstand usually is.

### 3.3 `MANUAL_CROSSINGS`

```python
dict(name="Tyre bridge", station_m=1043.0, kind="arch", brand="piretti")
```

`kind` `"footbridge"` lays `truss_bridge`; anything else lays `tyre_bridge`.
The kit has only those two spans plus the start and timing gantries, so a
hotel over the track can only be represented as a bridge; say so in the
comment. `brand` must be one of the kit's fictional brands (`apexsim`,
`brix`, `hexon`, `kronos`, `northwind`, `piretti`, `rolux`, `velocet`);
real sponsors are not in the kit and are silently dropped. An authored
crossing within 25 m of an OSM one replaces it.

### 3.4 `MANUAL_LANDMARKS`

```python
dict(kind="big_wheel", station_m=1010.0, side="right", offset_m=110.0)
dict(kind="blimp", name="Airship", station_m=900.0, side="right", offset_m=90.0,
     altitude_m=150.0, broadside_to_m=0.0, brand="piretti")
```

`kind` must be one `dress.rs` maps to a kit asset: `big_wheel`
(ferris wheel), `screen` (video screen), `stage` (fan-zone stage),
`camera_tower`, `tower` (control tower, the only "tall building" the kit
has), `floodlight`, `blimp`, `balloon`, `helicopter`. Anything else is
ignored without a message. `offset_m` is measured from the road edge.
`altitude_m` applies to sky kinds only (default 180 m over the road).

### 3.5 Changing the script itself

Prefer manual entries to code. A code change is justified only for a
generic rule that the existing five would also be right to receive (for
example: stadium seating tagged `leisure=stadium`, orchards as woodland, or
a street circuit whose road is `highway=primary` and cannot fit). If you
change `extract()` or the fit, re-run `python scripts/osm_layout.py --all
--offline` and confirm the five existing dossiers are unchanged (or explain
the diff), then `ats-dress --all --dry-run` and confirm they report the same
counts.

## 4. Verification

### 4.1 Idempotency

`ats-dress <Stem>.yaml` twice; `git diff --stat content/tracks/real/<Stem>.ats`
must be empty after the second run. Same for `ats-groom`.

### 4.2 Tests

`cargo test` in `track-editor` (dress, groom, export, underpass, catalogue).
The two whole-calendar bake tests are `#[ignore]`d (about twenty minutes);
`ats-export` on the new circuit already bakes it, so they are only needed
for a change to the bake itself (`cargo test -p track-core --test ue_export
-- --ignored`). Nothing per-track is expected to be added, but a script rule change (§3.5)
needs a test beside the existing ones.

### 4.3 Wall sanity scan

The wall bake writes every barrier, stand side, building side and pit wall
into `<Stem>.walls.msgpack` (`server/src/walls.rs`: `WallFile { version,
segments: Vec<WallSegment> }`; each segment has its two endpoints, base `z`,
`height_m` and material `kind`). A segment whose midpoint lies inside the
road width at its station, at road height, is scenery standing on the
track. There is no checked-in script for this yet; the first agent to need
it writes `scripts/check_walls.py` (load the msgpack, locate each midpoint
against the YAML centerline using the same nearest-point logic as
`osm_layout.Track.locate`, flag `|lateral| < half-width` with `|dz| < 2 m`)
and every later track runs it. Zero flagged segments is the gate; a handful
at the pit lane entry/exit tapers is a known cosmetic issue to list, not a
blocker.

### 4.4 Eyes on it

Two screenshots minimum (§2.4). Look for: pit building on the pit side of
the straight; stands facing the road, not their backs; no building or stand
on the tarmac; trees only where the real venue has them; bridges spanning
the road rather than standing beside it; the signature landmark present.
Attach the screenshots to the hand-back.

## 5. Known traps (found on the first five)

- **Overlapping API tiles deliver duplicate ways.** Already handled by id
  dedupe; do not "fix" it again.
- **Public-road circuits do not fit end to end.** Le Mans covers 41% of its
  centerline because the Sarthe is `highway=primary` in OSM; the script
  accepts ≥ 1.5 km matched at ≤ 3 m rmse in that case. Norisring, Melbourne,
  Montreal and Sochi are the candidates for the same problem (§6).
- **A stand within 20 m of the pit lane collides with the garages** the bake
  generates; dress drops it on purpose.
- **OSM `building=bridge` is a crossing, not a building** (a footbridge was
  once laid as a row of clubhouses across Silverstone's pit straight). Both
  the script and dress drop them now; a real bridge you want visible needs
  a `MANUAL_CROSSINGS` entry.
- **An OSM bounding box of an L-shaped building can reach onto the road**;
  dress pushes building rows back until the front is 3 m clear, and refuses
  ones that run across the road. A refusal means the outline is wrong for
  our purposes, not that the pusher is broken.
- **The bake's deep kit meshes pivot on the road-facing edge** and reach
  away from the road; only the control tower, camera tower, ferris wheel,
  tent and thin barriers are centred. Nothing to do per track, but it
  explains why a building "sits further back than the dossier says".
- **Sidecars are generated and gitignored.** After pulling a dressed track
  the server needs a fresh `ats-export --all` and a restart, or nothing
  stops a car at the barriers.
- **`-ApexTrack=` takes the stem** (`-ApexTrack=Budapest`, matched against
  the catalog row's `YamlBaseName`), else a substring of the lobby name,
  which is the YAML `name`: a parody (`Hungoverring`), never the real one.
- **`Track.locate` is nearest-point.** Where two parts of the lap run close
  together (Suzuka's crossover, IMS's infield beside the oval, Sepang's two
  straights sharing one stand) a feature can attach to the wrong leg with
  `station_m` and `side` both wrong. Check those by hand.

## 6. Per-track task cards

Each card gives: a seed bounding box `(min_lon, min_lat, max_lon, max_lat)`,
which is a starting point from the circuit's known position to be tightened
on the map, not trusted; what OSM is likely to give and what will need
manual entries; the circuit's own facts to source; and the risk to watch.
Stations are not given because they must be measured against the YAML;
corner and stand names are given so the agent knows what to look for.

Circuit facts below (stand names, pit side, landmarks) are from general
knowledge and must be **verified against the circuit's published spectator
map and satellite imagery**, not copied as-is.

### 6.1 Austin — `Austin.yaml` (Circuit of The Americas, 5.51 km)

- bbox seed: `(-97.652, 30.122, -97.626, 30.145)`.
- Pit lane: right of the start straight (garages on the inside of the loop
  under the main grandstand). The pit straight runs uphill into T1.
- Stands to source: Main Grandstand (pit straight, opposite the garages),
  Turn 1 (the hill), Turn 4, Turn 9, Turn 12, Turn 15 stands, Turn 19/20;
  the COTA spectator map lists them by turn number. Several are bleachers
  (`covered=False`, shallow).
- Landmarks: the 77 m observation tower behind the amphitheatre (inside
  T16–T18); OSM has it as `man_made=tower`; expect `kind="tower"`, which
  lays the control-tower asset (shorter than reality; note it). The Germania
  Insurance Amphitheater is a building.
- Woods: sparse scrub, `environment_type: plains`; expect few woods.
- Risk: the venue is big and OSM-dense; a single bbox may exceed 50k nodes.

### 6.2 Brands Hatch — `BrandsHatch.yaml` (GP loop, 3.91 km)

- bbox seed: `(0.250, 51.350, 0.275, 51.364)`.
- Pit lane: right of the Brabham Straight, garages on the inside; the
  Indy-loop link road at Surtees/Clearways shares raceway tags. The fit
  should still cover the GP loop; `raceway matched` will be below 1.
- Stands to source: Paddock Hill Bend stands, the Brabham Straight
  grandstands, Clearways/Clark Curve, Druids hill (natural banking, do not
  invent seating there). Names from the circuit's event maps.
- Crossings: the footbridge before Paddock Hill (`building=bridge` or
  `bridge=yes` footway); check it comes out as a crossing.
- Woods: the GP loop runs through woodland (Hawthorns, Westfield, Dingle
  Dell); the `country` scene should keep those and lose the invented belt on
  the Indy section's infield.
- Risk: the Indy loop's raceway may win the pit-lane candidate contest if
  a service road reaches the start line; verify `pit_lane.nodes`.

### 6.3 Budapest — `Budapest.yaml` (Hungaroring, 4.38 km)

- bbox seed: `(19.236, 47.572, 19.262, 47.588)`. The YAML has two
  `environment_type` keys (one at top level); harmless, leave it.
- Pit lane: right of the main straight; the pit building faces the Gold
  stands across the track.
- Stands to source: Super Gold / Gold 1–4 (main straight), Silver 1–5,
  Bronze 1–3 (the hillside natural terraces), the T1 stand, the T12–T14
  stands. The circuit's ticket map names them by colour; use those.
- Landmarks: none defining; the circuit sits in a bowl. Woods: hedgerows and
  small copses; `country`.
- Risk: many "stands" are terraces without a building outline, so expect
  most to be manual entries over long station spans with `depth_m` ≈ 8–10
  and `covered=False`.

### 6.4 Catalunya — `Catalunya.yaml` (Circuit de Barcelona-Catalunya, 4.66 km)

- bbox seed: `(2.246, 41.560, 2.275, 41.580)`.
- Pit lane: right of the main straight.
- Stands to source: Tribuna Principal (main), the lettered stands round the
  lap from the circuit's own map (B/C at T1–T2, F/G at the stadium section,
  the T10 stand, the T12–T13 "Estadi" stands). A run of these is one long
  continuous stand round the final sector; lay it as several spans, one per
  letter, so names survive.
- Crossings: the footbridge on the main straight and the one at T10.
- Woods: pine on the hillsides; only the dossier's woods matter now.
- Risk: the YAML may be the pre-2023 layout with the T14–T15 chicane; the
  fit tolerates the local deviation, but check `centerline covered`.

### 6.5 Hockenheim — `Hockenheim.yaml` (Hockenheimring, 4.57 km)

- bbox seed: `(8.552, 49.318, 8.582, 49.338)`.
- Pit lane: right of the start straight.
- Stands to source: the Motodrom bowl (Südtribüne, Nordtribüne, Innentribüne,
  Osttribüne: the stadium section from the Mobil 1 Kurve through the
  Sachskurve to the finish), the Mercedes-Tribüne at the hairpin, the
  Parabolika stands. Most are in OSM as `building=grandstand`; check they
  came through with names and depths (they are `_large_roof` class).
- Crossings: at least one footbridge on the Parabolika.
- Woods: the old forest section's woodland lines the outside of the Parabolika
  and T2–T6; `forest` is right, and the dossier should keep the infield
  bare.
- Risk: the old 6.8 km circuit's disused raceway ways may still be in OSM;
  if `raceway matched` is low but coverage is fine, ignore.

### 6.6 IMS — `IMS.yaml` (Indianapolis road course, 4.02 km)

- bbox seed: `(-86.245, 39.786, -86.224, 39.805)`.
- Pit lane: the oval's pit lane, right of the front straight, inside the
  oval. The road course uses the oval's front straight in the *opposite*
  direction to the oval; check the YAML's direction before trusting
  left/right.
- Stands to source: the oval's continuous stands (Paddock, Tower Terrace,
  Turn 1/2/3/4 stands, the Northwest Vista, the Pagoda plaza). Only the ones
  along the road course's route matter (front straight, T1 of the oval, the
  infield). Lay them as long spans; `depth_m` 25+ and `covered=True` where
  roofed.
- Landmarks: the Pagoda (control tower; `kind="tower"`), the scoring pylon
  (no kit asset: skip and note it).
- Crossings: the tunnels are under, not over; none expected.
- Woods: the infield golf course's trees; `country`.
- Risk: the oval is ~4 km of raceway that the road-course centerline does
  not explain; `raceway matched` ≈ 0.4 is expected. The infield section runs
  beside the oval's back stretch, so the nearest-centerline logic could
  attach oval-side stands to the wrong station. Check `side` per stand.

### 6.7 Melbourne — `Melbourne.yaml` (Albert Park, 5.30 km)

- bbox seed: `(144.955, -37.860, 144.985, -37.838)`.
- Pit lane: right of the start straight (pit building on the lake side).
- Stands to source: all temporary, named after drivers on the Grand Prix
  corporation's grandstand map (Fangio, Jones, Brabham, Prost, Senna, Clark,
  Waite, Webber, Schumacher, Piquet, Moss, Hill, Lauda, Stewart, Sadler).
  Nearly all will be manual entries; OSM may map none.
- Landmarks: Lakeside Stadium (a building); the skyline is not
  representable.
- Woods: parkland trees along the lake; `park`. The script only reads
  `natural=wood` / `landuse=forest`, so `leisure=park` tree cover yields
  few woods. Accept that or extend the rule generically (§3.5).
- Risk: the roads are public park roads. If OSM tags them `highway=raceway`
  the fit works; if not, this becomes a Le Mans-style partial fit or needs
  a script change. Report which. The YAML may predate the 2022 layout change
  (the T9–T10 chicane removal); coverage should still pass.

### 6.8 Mexico City — `MexicoCity.yaml` (Autódromo Hermanos Rodríguez, 4.30 km)

- bbox seed: `(-99.102, 19.396, -99.080, 19.413)`.
- Pit lane: right of the main straight.
- Stands to source: Main Grandstand (across the straight), Turn 1–3 stands,
  the T4–T6 stands, the T12 stand, and the **Foro Sol stadium section**
  (T13–T16). The stadium is tagged `leisure=stadium` / `building=stadium`
  in OSM, not `grandstand`, so the script sees it as a building at best.
  Represent it as manual stands along both sides of the stadium section
  with `depth_m` ≈ 25, `covered=False`, unless a generic stadium rule is
  added (§3.5).
- Landmarks: none in the kit's vocabulary beyond floodlights.
- Woods: the Magdalena Mixhuca park trees around the outer loop; `plains`.
- Risk: OSM has the old Peraltada banking and the baseball diamond; both
  should stay out (the Peraltada is not `raceway` anymore; verify).

### 6.9 Montreal — `Montreal.yaml` (Circuit Gilles Villeneuve, 4.36 km)

- bbox seed: `(-73.540, 45.490, -73.510, 45.515)`.
- Pit lane: right of the start straight, garages beside the Olympic
  rowing basin.
- Stands to source: the circuit numbers them (Grandstand 1/2 on the pit
  straight, 11, 12, 15, 21, 22, 24, 31, 33, 34, 46, 47); most are temporary
  bleachers (`covered=False`, `depth_m` 10–14). The Senna corner and the
  hairpin (T10) stands are the well-known ones.
- Crossings: the footbridges over the track near the pits and at the
  hairpin; the Pont de la Concorde passes over the circuit near the Casino.
  Check whether OSM's `bridge=yes` on that road lands as a `road` crossing.
- Landmarks: the Casino de Montréal (building).
- Woods: the island's parkland trees between the track and the basin.
- Risk: the rowing basin lies inside the bbox and is 2 km of water; make
  sure no building or wood ring is placed on it. Verify the raceway tags
  exist for the whole lap.

### 6.10 Moscow Raceway — `MoscowRaceway.yaml` (3.93 km)

- bbox seed: `(36.070, 55.955, 36.105, 55.972)`.
- Pit lane: right of the main straight.
- Stands to source: the main grandstand is the only permanent one; the
  circuit's published maps (DTM/WSBK era) show temporary stands at T1–T2
  and the T5–T6 complex.
- Landmarks: none. Woods: birch forest surrounds the venue; expect several
  `mixed`/`broadleaved` rings.
- Risk: OSM coverage of the venue may be thin (a raceway outline and little
  else). If the fit passes but the dossier is nearly empty, the task is
  mostly manual stands and correct woods; say so in the hand-back rather
  than inventing buildings.

### 6.11 Norisring — `Norisring.yaml` (2.30 km street circuit)

- bbox seed: `(11.110, 49.428, 11.132, 49.440)`.
- Pit lane: right of the start/finish straight in front of the
  Steintribüne; the pits are temporary awnings along the road.
- Stands: the **Steintribüne** (the monumental stone stand the circuit is
  built around; in OSM as a building / `historic`), plus temporary stands
  at the Grundig-Kehre and the Dutzendteich-Kehre. Represent the stone
  stand as a long `covered=False`, `depth_m` ≈ 30 stand (or a building row
  if the outline is not `grandstand`), and note which you chose.
- Crossings: none.
- Woods: the Dutzendteich lake's park trees.
- Risk: **this is a street circuit on public roads** (`highway=primary` /
  `secondary` in OSM, not `raceway`). The fit will probably refuse. Options,
  in order of preference: (a) check whether OSM has a `highway=raceway`
  relation or ways for the Norisring (some street circuits do); (b) add a
  generic fallback in `raceway_cloud()` that also accepts ways carrying a
  circuit's name, §3.5 applies; (c) if nothing works, report it as blocked
  with the fit numbers. Do not hand-georeference by eye.

### 6.12 Nürburgring — `Nuerburgring.yaml` (GP-Strecke, 5.14 km)

- bbox seed: `(6.930, 50.325, 6.965, 50.345)`. Keep it tight: the
  Nordschleife's 20 km of `highway=raceway` starts a few hundred metres
  north, and its link road is inside the GP complex.
- Pit lane: right of the start straight, in front of the Mercedes
  Tribüne.
- Stands to source: Mercedes-Tribüne (main, across the straight),
  T1–T4 stands round the Mercedes Arena, the Schumacher-S stands, the
  Bit-Kurve stand, the Dunlop-Kehre stands, the NGK-Schikane stand. OSM has
  most as grandstands.
- Landmarks: the ring°boulevard complex and the ring°werk are large
  buildings (expect `media_centre` rows).
- Woods: Eifel spruce all round; `forest`, expect `needleleaved`.
- Risk: the coarse fit may lock onto Nordschleife segments if they are in
  the bbox; the symptom is coverage far below 0.9. Tighten the bbox before
  touching the script.

### 6.13 Oschersleben — `Oschersleben.yaml` (Motorsport Arena, 3.70 km)

- bbox seed: `(11.265, 52.020, 11.295, 52.035)`.
- Pit lane: right of the start straight.
- Stands to source: the Haupttribüne (main), the T1 stand, the Hotel-Kurve
  stand, the Triple/Hasseröder-Kurve stands. The circuit hotel stands at
  the Hotel-Kurve (a real building; check it comes out as one and faces
  the road).
- Landmarks: none. Woods: flat farmland; expect few rings and accept a
  bare horizon; `plains`.
- Risk: none specific; likely the easiest of the set. Good first track for
  an agent to learn the pipeline on.

### 6.14 Sakhir — `Sakhir.yaml` (Bahrain International Circuit, 5.41 km)

- bbox seed: `(50.495, 26.020, 50.525, 26.045)`.
- Pit lane: right of the main straight; the Sakhir Tower stands behind the
  pits.
- Stands to source: Main Grandstand, Batelco Grandstand (T1), Turn 1 stand,
  University Grandstand (T10/11), Victory Grandstand (T14–15), the Oasis
  stands.
- Landmarks: the **Sakhir Tower** (`kind="tower"`, ten storeys; the kit's
  control tower is a stand-in, note it), and the floodlight masts of the
  night race. OSM often maps them as `tower:type=lighting` nodes; check the
  landmarks count. If none, author `floodlight` entries only where the real
  masts are, not as decoration.
- Woods: **none**, desert. The dossier's empty `woods` is what removes the
  invented tree belt; that is the point.
- Risk: OSM also holds the Endurance, Paddock, Inner and Outer layouts'
  raceway ways inside the bbox. Coverage of the GP centerline should still
  reach 0.9; `raceway matched` will be low. Verify the fit did not lock the
  centerline onto the Endurance loop (rmse would be high).

### 6.15 São Paulo — `SaoPaulo.yaml` (Interlagos, 4.31 km)

- bbox seed: `(-46.712, -23.712, -46.688, -23.695)`.
- Pit lane: right of the main straight (the Senna S drops away to the
  left).
- Stands to source: the lettered grandstands along the pit straight and the
  Senna S (A/B/D/G), the M stand at the Bico de Pato, the R stand at Junção,
  the newer grandstands at T4 and Descida do Lago. The circuit uses
  letters; use them.
- Landmarks: none in kit vocabulary; the pit complex is a big building row
  (expect `media_centre`).
- Woods: the two lakes' surroundings and the outer perimeter trees; the
  city is buildings outside the 140 m range.
- Risk: the circuit is counter-clockwise and hilly; the terrain heightfield
  comes from the YAML so nothing to do, but verify stands seat on the slope
  (screenshot the Senna S from the outside).

### 6.16 Sepang — `Sepang.yaml` (Sepang International Circuit, 5.54 km)

- bbox seed: `(101.725, 2.750, 101.752, 2.772)`.
- Pit lane: right of the main straight.
- Stands to source: the **Main Grandstand** with its canopy roof (one very
  long covered stand between the pit straight and the back straight, so it
  appears twice, once facing each straight; both `side`s must be right),
  the Hillstand (open mound at T1–T2), the K1/K2 stands at the hairpin, the
  T4 stand.
- Landmarks: the roof itself (the `_large_roof` family handles the look);
  floodlights (the circuit is lit).
- Woods: oil-palm plantation on every side. OSM tags it `landuse=farmland`
  or `landuse=orchard`, **not** forest, so `woods` will be nearly empty and
  the scene bare. Either accept the bare look (honest) or extend the wood
  rule generically to `landuse=orchard` with `leaf=broadleaved` (§3.5); say
  which.
- Risk: the double-sided main stand is inside 20 m of the pit lane on the
  pit-straight side and will be dropped there (correct); make sure the
  back-straight face is *not* dropped.

### 6.17 Shanghai — `Shanghai.yaml` (Shanghai International Circuit, 5.45 km)

- bbox seed: `(121.205, 31.328, 121.235, 31.352)`.
- Pit lane: right of the main straight; the pit building's team wings
  bridge the track near the end of the pit straight. Those should become
  **crossings** (author them in `MANUAL_CROSSINGS`; there are two, one at
  each end of the wing over the track).
- Stands to source: the Main Grandstand (huge, covered, across the pit
  straight; `_large_roof`), the T1–T3 "snail" stands, the T13 stand, the
  back-straight stands and the hairpin (T14) stand.
- Landmarks: floodlights (the venue is lit); the cantilevered wings above
  are the signature and can only be approximated by the bridge props.
- Woods: planted parkland; `plains`.
- Risk: the giant stands are in OSM as several parts; depths will be large
  (>25 m); expect many stand props. Check the chain of 40 m runs follows the
  curve of the T1 stand instead of cutting through it.

### 6.18 Sochi — `Sochi.yaml` (Sochi Autodrom, 5.85 km)

- bbox seed: `(39.945, 43.398, 39.972, 43.415)`.
- Pit lane: right of the main straight.
- Stands to source: the Main Grandstand (across the straight), T2 stand,
  the T3 (the long left round the Medals Plaza) stands, T4, T5, T10, T13,
  T15 stands; the circuit numbers them.
- Landmarks: the Olympic Park venues (Bolshoy Ice Dome, Fisht stadium,
  Iceberg arena) are buildings and will come out as `media_centre` rows
  where they are inside 140 m. The Olympic Cauldron is a `man_made`
  feature; if it lands as `kind="tower"` accept it.
- Crossings: several footbridges over the track; expect them from OSM.
- Woods: sparse landscaping; accept a bare scene.
- Risk: the circuit is public road round the Olympic Park; verify the
  raceway tags exist and that the fit covers ≥ 0.9.

### 6.19 Spielberg — `Spielberg.yaml` (Red Bull Ring, 4.32 km)

- bbox seed: `(14.752, 47.212, 14.780, 47.228)`.
- Pit lane: right of the start straight.
- Stands to source: Start-Ziel Tribüne (main), the Red Bull Tribüne at T1
  (Niki Lauda Kurve), the T3 (Remus/Schlossgold) stand, the T4 (Rauch)
  stand, the T9–T10 (Rindt/Red Bull Mobile) stands, the "Steiermark"
  stands; the circuit map names them. The T3 approach is a natural
  hillside; only author a stand where there is seating.
- Landmarks: the **Bull statue** is not by the Schlossgold pond as first
  guessed -- it is the rusted-steel bull in the fan zone beside the
  Mitte/Centre Grandstand (oversteer48.com's general-admission guide:
  "the massive steel Red Bull statue is in the Yellow Zone next to the
  Mitte / Centre Grandstand"). Not in OSM, so authored in
  `MANUAL_LANDMARKS["Spielberg"]` at the Tribuene Mitte stand's own
  station (3149.7 m, left), just beyond its outer edge. The kit now has
  a `statue` landmark kind / `misc/bull_statue` asset for it. The T1
  climb has a bridge over the track, a footbridge `crossing`.
- Woods: spruce on the hillsides; `forest`, `needleleaved`.
- Risk: the two hairpins are on steep slopes; screenshot the T1 climb from
  below to confirm the stand seats on the slope, not in the air.

### 6.20 Suzuka — `Suzuka.yaml` (Suzuka Circuit, 5.81 km)

- bbox seed: `(136.525, 34.835, 136.555, 34.855)`.
- Pit lane: right of the main straight (pit building under the Main
  Grandstand's south end).
- Stands to source: the Main Grandstand (V1/V2 blocks), the "S" curves
  stands (A/B), the Degner stand (C), the hairpin stand (D), the Spoon
  stand (G), the 130R stand (I), the Casio Triangle stand (Q). Suzuka
  letters its stands; use them.
- Landmarks: the **Ferris wheel** in Motopia (a `big_wheel`, OSM has it),
  the Suzuka Circuit Hotel (building). The crossover bridge is the track's
  own (`highway=raceway` + `bridge=yes`) and is **not** a crossing: the
  terrain/underpass code already models it. Do not author a bridge there.
- Woods: the circuit is in woodland on the east side; the dossier's woods
  fix the `plains` guess.
- Risk: the figure-8. `osm_layout.Track.locate` returns the nearest point on
  the centerline, so a stand near the crossover can attach to the wrong
  leg (`station_m` off by ~3 km, `side` flipped). Check every stand near
  the crossover by hand. The underpass export must still bake (see
  `track-editor/core/tests/underpass.rs`); run `cargo test` and the wall scan.

### 6.21 Yas Marina — `YasMarina.yaml` (5.55 km; verify it is the 2021 layout)

- bbox seed: `(54.590, 24.458, 54.618, 24.478)`.
- Pit lane: right of the main straight; the pit exit tunnel goes *under*
  the track (nothing to author).
- Stands to source: Main Grandstand (across the straight, huge, covered),
  North Grandstand, South Grandstand, West Grandstand, Marina Grandstand;
  the Abu Dhabi Hill is a natural mound (skip). The circuit map names them.
- Crossings: the **W Abu Dhabi hotel** spans the track between T13 and T14.
  OSM has it as a building, possibly `building=bridge` or `bridge=yes`.
  Author it as a `MANUAL_CROSSINGS` entry (kind other than footbridge gives
  the tyre-bridge asset; a poor stand-in, say so) so it is not laid as a
  building row across the road or silently dropped.
- Landmarks: floodlights everywhere (night race); check OSM's count. The
  marina and Ferrari World are out of range.
- Woods: **none**, desert and marina; the empty list removes the invented
  belt.
- Risk: OSM holds the old (pre-2021) T5–T7 chicane and the North/South
  short layouts as raceway; coverage should still pass. If the YAML is the
  old layout, note it; do not redraw the centerline.

## 7. Hand-back format

Report per track, in this order:

1. Fit line (rmse, coverage, matched) and the bbox used (tiles if split).
2. Dossier summary: corners / stands (osm + manual) / structures /
   crossings / landmarks / woods counts, pit side, and how the pit side was
   verified.
3. `ats-dress` report: placed counts, the full `skipped` list with a
   one-line disposition for each ("commentary box", "over the garages",
   "moved to crossings", …).
4. Idempotency, `cargo test`, wall scan: pass/fail with the output on
   failure.
5. Screenshots (which poses) and what was checked in them.
6. Anything you changed in `osm_layout.py` outside the three manual tables,
   with the before/after `--all --offline` counts for the five existing
   dossiers.
7. What is missing or approximated (a landmark with no kit asset, a hotel
   as a tyre bridge, orchards as no trees): the honest list.

## 8. Order of work

Do Oschersleben first (simplest), then the dense permanent circuits
(Hockenheim, Nürburgring, Catalunya, Spielberg, Shanghai, Sepang, Sakhir,
Yas Marina, Budapest, Austin, Brands Hatch, Moscow Raceway, São Paulo,
Suzuka), and the public-road ones last (Montreal, Sochi, Melbourne, Mexico
City, Norisring), because those are the ones most likely to need a generic
script change that the earlier tracks should not have to wait on. If a
script change becomes necessary, make it in a separate commit before the
track that needs it, re-check the five existing dossiers, and tell the
other agents.
