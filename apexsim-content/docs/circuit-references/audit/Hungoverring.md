# Hungoverring (Hungaroring) dressing audit

Sources read: dossier `circuit-references/Hungoverring.md`; maps
`hungaroring-gpticketshop-map-2027.jpg` (ticket map, new stand names),
`hungaroring-spectator-map-2026.jpg` (zones, every stand code),
`hungaroring-f1-2026-track.webp` (turn numbers); explain
the `ats-dress --explain` output; `Hungoverring.layout.json`; `MANUAL_STANDS["Hungoverring"]`
in `tools/osm_layout.py` (no MANUAL_LANDMARKS / MANUAL_CROSSINGS / CROSSING_KINDS entry).
`hungary-2026-wayfinding.pdf` read from the coordinator's render
(`hungary-2026-wayfinding.pdf p.1`): it places the Giant Wheel, stage,
fanzones and every giant screen.

Corner stations (from `Hungoverring.guide.yml`; the YAML has no corners and the layout
has a single "Hungaroring" corner): T1 638, T2 1108, T3 1342, T4 1816, T5 2028,
T6 2388 (T7 ~2440), T8 2612, T9 2728, T10 2936, T11 3098, T12 3538, T13 3818,
T14 4018; lap 4374. Main straight ~4100 -> 0 -> 638, pit lane right.

## Cells

| Column | Cell | Reason |
|---|---|---|
| S/F | dossier | Main covered stand is right (`stand Hungaroring: 7 x bay_10m_large_roof`, 269 m left, station 4350-245, OSM). Missing the rest of the straight's left side that both 2026 maps show: Grand Prix 1-2, Grid 1-3, Podium/Club (spectator map "P", "C"), Pit Exit 1-2, and T1 on the straight. Also big screens (dossier "Big screen opposite" GP/Apex) not laid. Secondary kit need: the 2025 pit building is 3+ storeys with Paddock Club over the lane and a roof terrace (dossier "Pit building (2025)"); the bake lays single-row `garage_6m` + roof `podium`. |
| Landmark | dossier | No unique structure beyond the fan-zone Giant Wheel and stage; the 2026 wayfinding map puts them in the blue zone outside T12 (by Fan 1-2 and Apex 2), not behind the main grandstand (dossier "Giant Wheel, Giant Swing, Dodgem, Stage", position was unconfirmed). `ferris_wheel` and `fanzone_stage` exist: add MANUAL_LANDMARKS (`big_wheel`, `stage`). Also the OSM `road` crossing at 971.7 is laid as `tyre_bridge` (explain "bridge (unnamed): truss_bridge, tyre_bridge"): add `CROSSING_KINDS["Hungoverring"] = {"road": "overpass"}`. |
| Stands | dossier | MANUAL_STANDS use the pre-2025 Silver/Bronze names and wrong spots (dossier pitfall "Use the new scheme"). `T12-T14 Stand` (3600-3950, centre (174.7, 71.6)) lies *inside* the T13 hairpin between its two legs (12->13 at (199,101), 13->14 at (153,34)) where no map has a stand. `Bronze 1` (1500-1700, T3-T4) has no stand on either 2026 map. The Fan stand is OSM-traced but laid as buildings (explain `building Silver 5: 2 x clubhouse` + the unnamed 3300 one). |
| Trees | ok | OSM woods (6, mostly `mixed`) planted as broadleaf/conifer/poplar mix plus 319 forest impostors; the dossier says nothing contrary (open amphitheatre valley with wooded edges). |
| Barriers | dossier | Dossier "Run-off": concrete blocks and fence on the left of T14 "up to the bridge" (2025) and debris fencing at T3, T11, T14 (2026): none mapped (layout barriers are fences round T1 and building walls); explain has no `concrete_4m_rail` at all. Add mapped wall/fence lines. Rule side: no catch-fence barrier kind (only `armco_4m_fence`). Tecpro 1193 modules (~2.4 km) looks high against an armco-dominated venue, worth a look. |
| Boards | rule | 415 hoardings fine, but only 10 marshal posts and 0 corner signs: the layout has one corner ("Hungaroring", display None) so `marshal_posts` falls back to every 400 m and `corner_signs` has nothing; the FIA map numbers far more posts and the 2026 corners are named. There is no MANUAL corner table in `osm_layout.py` to give a circuit its turns when OSM has none. |
| People | kit | GA hills behind T14 and T9-T11 (dossier; spectator map green/blue zones), full grandstands, marshals at posts. No figure kit. Wanted: seated crowd on the H/Grid/Apex stands (the `_crowd` twins cover only stands), standing spectators on GA grass banks (green zone T5-T11, blue zone T11-T14), marshals at the posts, a track-invasion crowd is not needed. |

## Positions from the maps

All sides are in the driving direction (clockwise lap); stations are this track's.

| Structure | Where | Side | Extent | Map |
|---|---|---|---|---|
| Grand Prix 2, Grand Prix 1 | start of the main straight just after T14, before Grid stands | left | ~4100-4170, two short stands (GP2 set back) | gpticketshop-2027, spectator-2026 (GP1, GP2) |
| Grid 2, Grid 1 (and Grid 3 per spectator map) | main straight between GP stands and the Hungaroring stand | left | ~4170-4340 | gpticketshop-2027, spectator-2026 |
| Podium (P) and Club (C) stands | between Grid stands and GP1/GP2, set back behind them | left | ~4130-4220 | spectator-2026 |
| Hungaroring + Hungaroring Platinum | as OSM (already laid) | left | 4350-245 | both |
| Pit Exit 2 | right after the Hungaroring stand | left | ~250-330 (current "Silver 1" 280-380 roughly here: rename) | spectator-2026 |
| Pit Exit 1 | between PE2 and T1 | left | ~340-440 | both |
| T1 | end of the straight, on the braking zone up to T1 | left (outside of T1) | ~450-620 (current "T1 Stand" is 600-760, past the apex: move back) | both |
| Chicane 2 | T5 exit on the run to T6 | left (outside) | ~2130-2230 | spectator-2026 (C2), gpticketshop (CHICANE 2) |
| Chicane 1, Chicane 3 behind/above it | approach to T6 | left | ~2240-2350, C3 one row further back (depth/offset) | spectator-2026, gpticketshop |
| Chicane 4 | at T6/T7 | left (outside) | ~2380-2440 | spectator-2026 (C4), gpticketshop (CHICANE 4) |
| Fan 1, Fan 2 | mid T11-T12 straight, two parallel rows | left | ~3270-3380 (OSM "Silver 5" 3352 + unnamed 3300, 50 m each) | gpticketshop (FAN), spectator-2026 (F1, F2) |
| Apex 1 | outside of T14 | left | ~4030-4100 (OSM unnamed structure at 4079 left, 50 x 10.5 m) | gpticketshop (APEX 1), spectator-2026 (A1) |
| Apex 2 | beyond the T12 braking zone, facing T12-T14 | left (outside of T12) | ~3530-3580, ~40-60 m back | gpticketshop (APEX 2), spectator-2026 (A2) |
| Remove: T12-T14 Stand | inside the T13 hairpin | - | 3600-3950 | no stand there on either map |
| Remove/rename: Bronze 1 | T3-T4 | - | 1500-1700 | no stand on either 2026 map |
| Giant Wheel (`big_wheel` -> `ferris_wheel`) | outside T12, between Fan 1-2 and Apex 2 | left of the T11-T12 run, ~60-80 m back | ~3480-3530 | wayfinding-2026 (Giant Wheel icon) |
| Stage (`stage` -> `fanzone_stage`), F1 Fanzone, giant swing, dodgem | beside the wheel, outside T12 | left, ~60-100 m back | ~3450-3560 | wayfinding-2026 |
| Second F1 Fanzone | behind the Hungaroring/Platinum stand | left, ~80-120 m back | ~0-150 | wayfinding-2026, spectator-2026 |
| Giant screens (`screen` -> `video_screen`) | inside the straight on the pit side opposite T1/PE stands (3), on the pit building facing H (2), opposite the Grid/GP stands (3); infield at T4-T5 (2) and inside T11-T12 (1) | right on the straight; inside elsewhere | ~400-600, ~50-200, ~4150-4300 right; ~1900-2050, ~3250 inside | wayfinding-2026 (Giant Screen icons) |
| Podium, Club stands | behind Grid 1-3 / GP2 rows, second row | left | ~4130-4230 | wayfinding-2026 (PODIUM, CLUB, GRAND PRIX 1 behind GRID 1-3, GRAND PRIX 2) |
| Road bridge over the track | T1-T2 leg | across | station ~972 (OSM crossing) | gpticketshop (bridge symbol on T1-T2) |
| GA zones | green: outside T5-T11; blue: outside T11-T14; yellow: behind the main straight stands | left mostly | ~2000-3100, ~3100-4050 | spectator-2026 |

## Kit gaps

- `building/pit_building_tiered` (or a `garage_6m_upper` storey module): a 3-storey pit
  building with a glazed hospitality floor over the garages and a roof terrace (Paddock
  Club over the pit lane). Also wanted at Interlagos and Le Mans.
- Figures (People column): `people/crowd_bank_10m` (standing spectators on grass),
  `people/marshal_pair`.

## Rule gaps

- No manual corners table: a circuit whose OSM has no named corners gets no corner signs and
  only 400 m marshal posts (`dress::marshal_posts`, `corner_signs`).
- Crossing kind `road` defaults to `tyre_bridge` (`dress.rs` crossing match `_ =>`):
  any OSM road bridge on a circuit without a `CROSSING_KINDS` row becomes a tyre arch.
  Safer default: `overpass_concrete`.
- No catch-fence barrier kind (`catch_fence_6m` exists in the Fence kit, `barriers::decide` never emits it).
- GA spectator banks are only laid for `fan_dressing` styles (Nordschleife) and only from
  parking/camp areas; no dossier entry type for a GA zone.
## S/F dossier pass

In the tables:

- Removed: Silver 1, T1 Stand.
- `MANUAL_STANDS`: Grand Prix 1 (4090-4148 left); Grid 2 (4155-4210 left); Grid 3 (4215-4270 left); Grid 1 (4276-4335 left); Grand Prix 2 (4088-4146 left); Club (4152-4210 left); Podium (4216-4300 left); Pit Exit 2 (310-385 left); Pit Exit 1 (405-490 left); T1 (505-590 left).
- `STAND_COVER`: Hungaroring covered.
- `MANUAL_LANDMARKS`: screen at 4180.0 right; screen at 4240.0 right; screen at 4300.0 right; screen at 345.0 right; screen at 450.0 right; screen at 550.0 right.

Left, or for later columns:

- Geometry (edge_run, read-only): every new front is 10 m (30 m second row) off the left edge, on the left all the way, 18-56 m from the pit lane nodes (lane is right, lateral -5..-22 m). No overlap with OSM "Hungaroring" (4350-245).
- Front/back row order of Grand Prix 1/2: the spectator and ticket maps put GP1 in front and GP2 behind; the wayfinding map draws GRAND PRIX 2 in the front row and GRAND PRIX 1 behind. Two maps against one: GP1 in front.
- Not placed: the wayfinding map's two giant screens on the pit building facing the Hungaroring stand (~5 and ~155 right). They stand where the bake lays garages; no screen-on-the-pit-building rule or kit piece.
- Hungaroring Platinum is the T14 end of the OSM "Hungaroring" outline, not a separate stand; left as one.
- Left for other columns: Apex 1 (outside T14, ~4030-4100) and the rest of the audit's Stands rows (Bronze 1, T12-T14 Stand removals, Chicane 1-4, Fan, Apex 2); the F1 Fanzone behind the Hungaroring stand (~0-150 left, 80-120 m back); the infield giant screens at T4-T5 and T11-T12.
- Kit gap unchanged: the 3-storey pit building with the Paddock Club over the lane (building/pit_building_tiered).

## Landmark dossier pass

In the tables:

- `MANUAL_LANDMARKS`: the fan zone outside T12, Giant Wheel (3495 left, 70 m) and stage (3530 left, 90 m), 2026 wayfinding map, affine fit 6-43 m.
- The road crossing at 971.7 is real: a concrete overpass by R4. The footbridge at 274.3 is real.

Left, or for later columns:

- The stage may meet Apex 2 when the Stands column lays it: then 3460 left, 90 m.
- The giant swing and dodgems: no kit piece.

## Stands dossier pass

In the tables:

- Bronze 1-3 and the T12-T14 stand inside the T13 hairpin gone; Chicane 1-4 on the T5-T6 run; Fan 1 under its OSM name "Silver 5", Fan 2, Apex 1, Apex 2; four banks over T9-T12.
- `DROP_STRUCTURES`: the unnamed outlines where Fan 2 and Apex 1 stand (the stands themselves).

Left, or for later columns:

- Apex 1's first run may meet Grand Prix 2 near 4088 on the T14 bend: then 4045-4075.
