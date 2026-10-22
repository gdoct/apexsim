# Circuit dressing to the references

How every circuit in `tracks/default/` is brought up to its real venue's
look, and where each one stands. The plan and its check are roadmap
package 29 ([apexsim/docs/roadmap.md](../../apexsim/docs/roadmap.md)); this
doc holds the method and the status table.

The references are the dossiers in
[circuit-references/](circuit-references/README.md): one per stem, the
real venue down to building level, with the maps listed in
`<Stem>.sources.json` and fetched into `docs/circuit-references/maps/<Stem>/`.
They name real circuits, corners and brands; what reaches the screen
follows [naming.md](naming.md) (`display_name`, `CORNER_DISPLAY`, a prop's
`text`).

## What is improved

Each category below is a column of the status table.

| Column | What it covers | Today's kit |
|---|---|---|
| **S/F** | The start/finish straight as the venue's focal point: pit building and its roof, main stands facing the garages, gantry, screens, flags, the podium, crowd areas | `pit` complex (export), `grandstand` bays and `_crowd` twins, `bridge/start_gantry`, `attraction/video_screen`, `sign/flag_pole` |
| **Landmark** | The venue's unique structures (the tower at Austin, the bull at Spielberg, the tyre bridge at Le Mans, the Ferris wheel) | `dress::LANDMARK_ASSETS`, `bridge/tyre_bridge`, `attraction/ferris_wheel`, `building/observation_tower`, the Marina Bay landmarks |
| **Stands** | Grandstands and spectator areas: size, shape, roof, place by corner and side; big screens with them | stand bays and the street family, `scaffold_10m`, `banking_seats`, `spectator_bank_10m`, `video_screen` |
| **Trees** | Species, density and placement of the woods and belts | card trees, near-LOD trees, `forest_impostor[_conifer]`, dossier woods with leaf type |
| **Barriers** | Barrier and fence kind and line per cell: armco, concrete, Tecpro, tyres, catch fence | `barriers::decide` per 4 m cell, groomed runs |
| **Boards** | Hoardings, brands, flags and banners, corner signs, marshal posts, trackside equipment, car parks, camps | boards, `sign/flags/*.png`, `gen_brands.py`, `dress::surroundings` |
| **People** | Static figures: spectators on banks and gates, marshals at posts | none today except the `_crowd` stand variants (crowd cards); a new kit family |

The order of work is by column, every track at once: S/F first (the largest
visible win, and where showcases and site shots look), then Landmark,
Stands, Trees, Barriers, Boards, People last. People is a new kit family
plus client instancing, scoped to static figures; it is the final step and
nothing before it waits on it.

## Three kinds of change

Dressing is generated: `ats-dress` and `ats-groom` lay everything from the
dossier and the kit, and a re-run discards what was placed by hand
([track-pipeline.md](track-pipeline.md#dressing-ats-dress)). Every
improvement is therefore one of three edits, and the status table says
which:

1. **Dossier** (`<Stem>.layout.json` via `tools/osm_layout.py`): a stand in
   `MANUAL_STANDS`, a landmark in `MANUAL_LANDMARKS`, a crossing in
   `MANUAL_CROSSINGS`, a wood with its leaf type, a barrier kind, a
   surroundings layer. Positions come from the trace page
   (`tools/trace_layout.py <Stem> --open`, click to trace). A change to one
   circuit's tables must leave the others unchanged: run
   `python tools/osm_layout.py --all --manual-only --dry-run` and read
   "unchanged" for every other stem.
2. **Dress rule** (`track-editor/core/src/dress.rs`, `barriers.rs`,
   `groom.rs`): a landmark kind mapped to an asset, a barrier decision, a
   surroundings sub-layer, a stand family threshold. A rule change touches
   every track: re-dress `--all` and read the placed counts against the
   commit before.
3. **Kit asset** (`props/<kind>/<asset>.glb` from a generator under
   `tools/generators/props/`, a `props::KIT` row with its footprint, the
   matching entry in the client's `kKinds`, then `ApexPropImport`):
   a structure no kit piece can stand in for. Asset keys and `text` carry no
   trademark ([naming.md](naming.md)).

## The audit

Column by column, a track's cell is filled from three sources read against
each other:

- the dossier's "Buildings and structures", "Start/finish, pit lane,
  paddock" and "Modelling pitfalls" sections, and the index's "what to
  check" column;
- the fetched maps, read as images (the wayfinding, grandstand and circuit
  maps; `circuit-references/render_maps.py` turns the PDFs into pages) to fix
  each structure's corner, side and extent where the text only names it.
  What a map adds goes into the dossier's `MANUAL_*` tables, and the dossier
  page gets the position with its map as source;
- `ats-dress --explain <yaml>`: what each dossier entry became, what was
  skipped and why, and what no pass owns.

A cell then reads **ok**, **dossier**, **rule** or **kit** (the kind of
change it needs, the smallest that does it), or **-** where the real venue
has nothing in that column. A cell with a kit need is also a row in the
[kit gaps](#kit-gaps) list, so assets are built once for every track that
needs them.

Each track's reasons are in `circuit-references/audit/<Stem>.md`: the
cells with their sources, then the positions from the maps (each structure
the maps place and the dossier lacks or misplaces, by station, side and
extent, with the map it came from), its kit gaps and rule gaps. That page
is the worklist for the track's dossier edits: a position moves into the
`MANUAL_*` tables and the dossier text when its column is worked, and
leaves the audit page then.

## Checks

- **Dossier and placement:** the trace page for a position; `ats-dress
  --explain` for what was laid and skipped; `--all --manual-only --dry-run`
  for the other dossiers.
- **On screen:** the same spots before and after, from the unattended
  screenshot loop (`-ApexAutoRace`, `-ApexScreenshotAfter`,
  `-ApexCameraLookAt=X,Y,Z,TX,TY,TZ`,
  [apexsim/docs/game/cameras.md](../../apexsim/docs/game/cameras.md)); a run
  per track in `scripts/verify/runs.yml` with its S/F and landmark cameras,
  so a column's pass over all tracks is one runner invocation. A person
  judges the shots before the cell is closed.
- **Barriers are a sim change:** barriers and buildings bake into
  `walls.msgpack` and the server stops cars on them. A barrier-column change
  is judged with the AI survey (`survey_ai_races_on_every_circuit`) against
  the commit before, on both backends, like any wall change.
- **Blast radius:** a dress run rewrites every `.ats` it touches, and a
  changed scene needs `ats-export` and the catalogue (`bake.ps1`); the
  showcases and guides of a changed track go stale. Work is batched per
  column and committed per track.

## Working a column

How the S/F column was done, and how the next ones go:

1. **Rules first.** The column's rule gaps (below) in `dress.rs`,
   `groom.rs` or `osm_layout.py`, with a unit test each. Nothing is
   re-dressed yet.
2. **Dossier edits per circuit**, from its audit page's positions and the
   maps (`circuit-references/render_maps.py` for the PDFs; needs
   `pip install pymupdf`). The entries go into `osm_layout.py`'s tables
   (`MANUAL_STANDS`, `STAND_COVER`, `MANUAL_LANDMARKS`, ...), each with a
   comment naming its map. `python tools/dossier_check.py <Stem>` then
   re-applies the tables (`--manual-only`; Marina Bay, with no DEM
   sidecar, from the cached extract with `--offline`), flags a
   `STAND_COVER` key that matches no stand, prints what the dress pass
   lays differently and lists structures the dossier lost. The circuit's
   audit page gets a "<Column> dossier pass" section: what went in, what
   is left. The drafting parallelises (one agent per few circuits, writing
   proposals, not files); applying does not, since every circuit's tables
   are in one file.
3. **One bake and one survey for the column.** Before the bake, the AI
   survey on the content as it is (`cargo test --release --test all
   ai_race_start_test::survey_ai_races_on_every_circuit -- --ignored
   --nocapture` in `apexsim/server/sim`, a scratch `CARGO_TARGET_DIR`
   when a server runs). Then `& .\bake.ps1 -Release -Stage
   dress,export,previews` from `apexsim-content/` in PowerShell (not
   through `powershell -File`, which passes the stage list as one string),
   `ats-dress --all` once more to see that nothing changes, the
   track-editor tests, and the survey again;
   `python scripts/survey_compare.py before.txt after.txt` (in
   `apexsim/`) compares them track by track. Stands, buildings and
   barriers are walls on the server, so any column can move it.
4. **Shots.** A run per circuit in `apexsim/scripts/verify/runs.yml`
   (`c5-sf-<stem>`: four poses of the straight, made with
   `scripts/verify/spots.py`'s `Lap` and `look_at`; a column that is not
   at the straight needs poses of its own), run with `run_checks.py
   "c5-sf-*"` and judged by a person. A cell becomes `ok` when its shots
   pass.

Traps met on the S/F and Landmark passes:

- An authored landmark stands for the OSM structure whose outline (plus
  5 m) holds its centre, and that row is dropped: read
  `dossier_check.py`'s "lost structure" lines. A big building OSM maps
  (an arena, a casino) is placed by anchoring the landmark on its
  outline's centre with `at=(lon, lat)` and its yaw.
- `MANUAL_STRUCTURE_ASSETS` applies only on a full extract, and only
  MandarinaBay and Shiverstone have one cached: elsewhere it waits for an
  online refetch, which rebuilds every surveyed layer.
- `dossier_check.py` runs `track-editor/target/release/ats-dress`: rebuild
  it after a `dress.rs` change, or new kinds are dropped silently.
- `--manual-only` rebuilds the `MANUAL_STANDS` stands but keeps one whose
  entry is unchanged byte for byte; an OSM stand cannot be moved, dropped
  or renamed from the tables (R18), only roofed by name or by station.
- `osm_layout.py` writes CRLF on Windows and git stores LF, so a
  re-written dossier shows as whole-file churn in the working tree only.
- Python on the Windows console prints cp1252: set `PYTHONIOENCODING=utf-8`
  before piping track names into a YAML or JSON file.

Open decision for the owner: whether the four circuits without a dossier
are removed from the calendar.

## Status

A cell holds the kind of change it needs, `ok` when it matches the
reference, `-` when the venue has nothing there; where a cell needs more
than one kind, it names the main one and its audit page the rest. The
[rule gaps](#rule-gaps) that touch every track (concrete, corners)
are not repeated per cell. The order of columns is the order of work.

| Stem | S/F | Landmark | Stands | Trees | Barriers | Boards | People |
|---|---|---|---|---|---|---|---|
| [AlbertParkour](circuit-references/audit/AlbertParkour.md) | ok | ok | ok | ok | ok | ok | ok |
| [Armadillos](circuit-references/audit/Armadillos.md) | ok | ok | ok | ok | ok | ok | ok |
| [Bahrainless](circuit-references/audit/Bahrainless.md) | kit | ok | ok | ok | ok | ok | ok |
| [Barcelunatic](circuit-references/audit/Barcelunatic.md) | ok | ok | ok | ok | ok | ok | ok |
| [BrandsScratch](circuit-references/audit/BrandsScratch.md) | ok | ok | ok | ok | ok | ok | ok |
| [ChillyVilleneuve](circuit-references/audit/ChillyVilleneuve.md) | ok | ok | ok | ok | ok | ok | ok |
| [Gnocchi](circuit-references/audit/Gnocchi.md) | ok | kit | ok | ok | ok | ok | ok |
| [Hockeyheimring](circuit-references/audit/Hockeyheimring.md) | ok | ok | ok | ok | ok | ok | ok |
| [Hungoverring](circuit-references/audit/Hungoverring.md) | ok | ok | ok | ok | ok | ok | ok |
| [Interlaggos](circuit-references/audit/Interlaggos.md) | ok | - | ok | ok | ok | ok | ok |
| [Lemons](circuit-references/audit/Lemons.md) | ok | ok | ok | ok | ok | ok | ok |
| [MandarinaBay](circuit-references/audit/MandarinaBay.md) | rule | ok | ok | ok | ok | ok | ok |
| [Monzarella](circuit-references/audit/Monzarella.md) | ok | kit | ok | ok | ok | ok | ok |
| [Mordschleife](circuit-references/audit/Mordschleife.md) | dossier | ok | ok | ok | ok | ok | ok |
| [Nuerburgerring](circuit-references/audit/Nuerburgerring.md) | ok | ok | ok | ok | ok | ok | ok |
| [Oskarsleben](circuit-references/audit/Oskarsleben.md) | ok | - | ok | rule | ok | ok | ok |
| [RedPullRing](circuit-references/audit/RedPullRing.md) | ok | ok | ok | ok | ok | ok | ok |
| [Shanghaied](circuit-references/audit/Shanghaied.md) | ok | ok | kit | rule | ok | ok | ok |
| [Shebang](circuit-references/audit/Shebang.md) | ok | ok | ok | ok | ok | ok | ok |
| [Shiverstone](circuit-references/audit/Shiverstone.md) | ok | ok | ok | ok | ok | ok | ok |
| [Snorisring](circuit-references/audit/Snorisring.md) | ok | ok | ok | ok | ok | ok | ok |
| [SpaFrankenchamps](circuit-references/audit/SpaFrankenchamps.md) | ok | ok | ok | ok | ok | ok | ok |
| [Sudoku](circuit-references/audit/Sudoku.md) | ok | ok | ok | ok | ok | ok | ok |
| [Zandervoort](circuit-references/audit/Zandervoort.md) | ok | ok | ok | ok | ok | rule | ok |

Endianapolis, HermanosRodriQueso, MoscowMule and YasMarinara have no
layout dossier (their OSM fits fail; `BBOXES` in `tools/osm_layout.py`
says why) and are not dressed. They are outside this work, and removing
them from the calendar is the owner's option.

**S/F column.** Done for every circuit but three: the dossier pass (each
audit page's "S/F dossier pass" lists what went in and what is left) and the
`c5-sf-*` shots, judged 2026-10-21 (Gnocchi's main stand, `stadium` from
the Stands pass, with the `c5-st-*` shots). Open: Bahrainless (the
tent-fabric roofs, kit gap 4), MandarinaBay (the screens on the pit
building's face, R20) and the
Nordschleife, whose start/finish needs the trace page. The multi-storey pit
buildings (kit gap 2, with R12 and R20) stay with their kit row.

**Landmark column.** Done but for two circuits: the rules, the dossier
pass (each audit page's "Landmark dossier pass"), the bake, the survey
(identical) and the `c5-lm-*` and `c5-r14-*` shots, judged 2026-10-22.
Open: Gnocchi (the Fisht stadium has no kit piece) and Monzarella (the
banking's curves beside the road have no kit piece).

**Stands column.** Done but for one circuit: the rules (stand families,
`STAND_FAMILY`, `DROP_STRUCTURES`, stands clear of the run-off), the dossier
pass ("Stands dossier pass": stands OSM maps as buildings, bleachers and
wrapping outlines entered in `MANUAL_STANDS` by hand), the bake, the survey
(identical but for noise) and the `c5-st-*` shots, judged 2026-10-22.
Open: Shanghaied (Grandstand A's lotus-leaf roof has no kit piece).

**Trees column.** Done but for three circuits: the rules (species by
climate, woods past the belt, scrub, `WOOD_LEAF`), the dossier pass ("Trees
dossier pass"), the bake, the survey (identical) and the `c5-tr-*` shots,
judged 2026-10-22. Open: Oskarsleben and Shanghaied (tree rows, R8).

**Barriers, Boards and People columns.** Done but for one cell: the rules
(concrete by the pit lane and against mapped walls, street/park/club rails;
`MANUAL_CORNERS` and a post every 150 m; marshals at the posts and groups
at the fan zones), the Boards dossier pass, the bake, the survey (identical
but for the Nordschleife's guard rail) and the `c5-bp-*` shots, judged
2026-10-22. Open: Zandervoort's Boards (the board sets at Hans Ernst and
T9/T10, R17).

The `c5-*` runs stay in `scripts/verify/runs.yml` to shoot a circuit again
after a later edit.

What the audit says across the table:

- **S/F** needed a dossier edit almost everywhere: the main stands opposite
  the pits misplaced, missing or open where the real ones are covered,
  and no screens. Ten pit buildings are multi-storey where the bake lays
  one row of garages (kit gap 2).
- **Stands** is a dossier edit at 22 tracks: stands misplaced by hundreds
  of metres (Montreal's hairpin stands at T8-T9, Zandvoort's Hairpin
  stand at Hans Ernst, Albert Park's 800-1400 m early), on the wrong side
  (Sepang's main stand, Spa's Bus Stop, Albert Park's Moss), real stands
  laid as building blocks because OSM tags them as buildings
  (Silverstone, Le Mans), and the general-admission banks absent (R2).
- **Barriers**: the audit found concrete laid nowhere but the
  Nordschleife and one style for every circuit; both are rules now
  (concrete beside the pit lane and against mapped walls, the street,
  park and club rails), and no cell needs a dossier edit.
- **Boards**: the corner signs and marshal posts follow the dossier's
  named corners, and ten dossiers have none or bogus ones (R10).
- **People**: the figure kit is laid: the banks'
  crowds from the Stands pass, a marshal at every post, knots of
  spectators round the fan zones' stages, wheels and food stalls.

### Kit gaps

Assets the audit finds no kit piece for, one row each, with the tracks
that need it. Built once, before the per-track dossier edits that place
them. A row whose asset is built says so and names the keys; it leaves the
list when the tracks it names have it placed. Unbuilt keys are working
names.

| # | Asset (kind/key) | Stands in for | Tracks |
|---|---|---|---|
| 2 | built and stacked by the exporter (`CircuitStyle::pit_storey_for`): `pit/garage_upper_6m`; roof screens are R20 | multi-storey pit buildings | BrandsScratch, ChillyVilleneuve, Gnocchi, Hockeyheimring, Hungoverring, Interlaggos, Lemons, Monzarella, Nuerburgerring, Oskarsleben |
| 4 | built: `grandstand/tent_roof_10m`, `pit/garage_roof_membrane_6m` (stacked at Bahrainless) | white membrane roofs | Bahrainless |
| 10 | built: `building/arena_dome`, `misc/cauldron` | Olympic ice arenas, the Olympic cauldron | Gnocchi |

Smaller asks, built only if a column's shots call for them: an amphitheatre canopy (Armadillos), a raised viewing deck
(Interlaggos), stone block towers (Snorisring), a lit light truss
(MandarinaBay), the Steilstrecke ramp (Mordschleife).

### Rule gaps

Dress, groom and dossier-tool rules the audit finds wanting, with the
tracks they show at. Each is a rule change: re-dress `--all` and read the
counts against the commit before. A built rule leaves the list; the labels
stay as they are.

- **R8. Tree rows.** OSM `natural=tree_row` ways (Oskarsleben's 15,
  Shanghaied's 13) are never extracted, so the rows that edge an open
  bowl or an access road are missing; the extract needs a refetch, and
  only MandarinaBay and Shiverstone have one cached.
- **R12. The podium sits on the middle garage** and roof screens have no
  height: the Bahrainless and Sudoku podiums are at one end, the Sudoku
  screens on the pit roof.
- **R16. Water.** `WATER_STEMS` (the extract's water bodies from OSM
  relations) is MandarinaBay alone; Albert Park's lake comes from
  `MANUAL_WATER`. Other circuits' lakes and channels (Montreal's, the
  Norisring's Dutzendteich) are only the small water `areas`.
- **R17. Checks.** Nothing flags a stand laid on the far side of its
  straight (the Shebang main stand) or a distance-board set missing at a
  heavy braking zone (Zandervoort's Hans Ernst and T9/T10).
- **R20. Screens on the pit building's roof or track face** have no rule:
  a `screen` landmark stands on the ground, and one in the garage zone is
  dropped (AlbertParkour, Armadillos, Bahrainless, Hungoverring,
  MandarinaBay, Sudoku); with R12 (the podium) it is the pit building's
  share of kit gap 2.
- **R21. Pit lanes that stop short.** AlbertParkour's `MANUAL_PIT_LANE`
  is 0-380 where the real lane runs from about 4760 to 140, and Lemons'
  lane is 347 m against the real one before the Ford chicane to the
  Dunlop curve; a lane change moves the server's pit and needs the pit
  survey.

Found on the way and outside dressing (track geometry, georeference,
references): in [follow-ups.md](../../apexsim/docs/follow-ups.md#6-circuit-dressing).
