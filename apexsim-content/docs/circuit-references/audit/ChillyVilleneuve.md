# ChillyVilleneuve (Circuit Gilles Villeneuve) — dressing audit

Sources: `circuit-references/ChillyVilleneuve.md`, README row; maps
`oversteer48-grandstand-map-2023.jpg`, `oversteer48-ga-viewpoints-2026.jpg`,
`fia-2024-circuit-map-pit-lane.pdf` (pages 2-3 rendered: circuit map with marshal
posts, pit lane drawing with 43 boxes); the `ats-dress --explain` output; layout.json;
`osm_layout.py` MANUAL_STANDS (13 entries), MANUAL_LANDMARKS (Casino tower at 2350).

Stations (layout has **no named corners**, only one OSM name; corners below from the centerline
heading change, numbered against the FIA map): line 0, lap 4346. T1 ~240 (L), T2 Senna hairpin
300-390 (R), T3-T4 690-780, T5 900-1050, T6 1230-1260 (L), T7 1290-1380 (R), T8 1980-2010 (R),
T9 2036-2096 (L), **T10 hairpin 2640-2720 (R)**, T11 2780-2810 (L), T12 ~2850-2880,
Casino Straight ~2880-3850, T13-T14 chicane 3860-3920. Pit lane left, 827 m; garages end at the
line (FIA pit drawing, "Pole LHS"), pit exit road runs round the outside of T1-T2.

## Cells

| Column | Cell | Reason |
|---|---|---|
| S/F | dossier | Pit complex generated on the left (ok); the 2019 three-storey paddock building ("Paddocks", 330 m hangar footprint) is skipped as pit complex, so the garages stand without the building's upper floors, terrace or the three roof screens facing GS1 (dossier "Paddock building (2019)"). Grandstand 1 is short and misplaced (77 m straddling the line; maps put ~200 m before it). Platine (the only covered stand) is missing; MANUAL "Grandstand 2" (30-170, 135 m, open) stands where Platine is. Main fix is dossier; upper floors are a rule/kit item. |
| Landmark | kit | Casino de Montreal and the Biosphere have no kit piece; MANUAL uses a control_tower for the Casino (and at 2350, which is the T9-T10 back straight, not the Casino Straight). 3 more OSM `tower`s (Tour de depart 328, 371, 3048 — rowing-basin towers) laid as control_tower: plausible. Pont de la Concorde (1920.5, T7-T8) is laid as `tyre_bridge` (kind `road`): should be a road bridge (`overpass_concrete` / `viaduct_deck`) — dossier CROSSING_KINDS. Explain: "skipped building Pavillon du Canada: reaches across the road" (1443 right, 83x78 m footprint). |
| Stands | dossier | The hairpin group is ~600 m early: MANUAL GS21/22/15/34/24/47/46 at 1970-2290 sit at T8-T9 (2000-2100), but both oversteer48 maps place them at T10-T12 (2560-2890). GS46/GS47 sides are swapped. GS34 is inside the hairpin, not outside. GS11 is lost: "skipped stand Grandstand 11: nothing to build clear of the pit lane" (pit exit road round T1-T2; needs a gap_m behind it). GS10 (2026) and GS32 missing; Platine missing. |
| Trees | dossier | 25 woods all `mixed`; only ~220 real trees laid (rest are grass/wildflower clumps) on a park circuit the dossier calls tree-lined most of the lap (Parc Jean-Drapeau; "trees block views from several stands"). Needs MANUAL_WOODS (broadleaved belts) along T3-T7 and the T8-T10 back straight. |
| Barriers | rule | Groom: 1324 armco, 355 armco_fence, 313 Tecpro, 297 tyres, **0 concrete**. The real circuit is concrete walls with catch fence almost everywhere (dossier pitfalls: "walls close everywhere", Wall of Champions). 28 OSM `wall` lines are mapped but `decide` only takes a mapped wall when within 5 m and run-off <= 4 m. Needs a park/semi-street style (concrete + catch fence) or a looser wall match. |
| Boards | dossier | 491 hoardings, 12 distance boards ok, but **0 corner signs and only 9 marshal posts** for 4.3 km: the layout has no named corners (OSM has none; no MANUAL corner table exists in osm_layout.py). FIA map numbers posts M1-M14 (incl. half-posts, ~24). Car parks P1-P12 ok. |
| People | kit | GA viewpoints (blue on `oversteer48-ga-viewpoints-2026.jpg`) beside GS1, at T6, T9, the hairpin approach and T11-T12; marshals at ~24 posts. Wanted: standing crowd behind fences, marshals. |

## Positions from the maps

Scale on the oversteer48 maps ~1.75 m/px (Casino Straight T12-T13 ~1000 m). Sides are driving the lap.

| Structure | Where | Side | Extent | Map |
|---|---|---|---|---|
| Grandstand 1 | main straight before the line, T14 exit to the line (stepped, tall end at the line) | right (infield) | ~4140-4340, ~200 m | both oversteer48 |
| Platine (covered) | between line and T1, set back ~20-30 m | right | ~80-150, ~60 m, covered | both oversteer48 |
| Grandstand 2 | not on either map; MANUAL 30-170 overlaps Platine | — | replace by Platine or verify | — |
| Grandstand 12 / 11 | Senna T2 outside, 12 at entry, 11 at exit | left | 12 ~250-320, 11 ~330-420, behind the pit-exit road (gap ~25 m) | both |
| Grandstand 10 (new 2026) | T2 exit to T3 approach | left | ~420-480 | ga-viewpoints-2026 |
| Family (33) | east of T6-T7, inside T7 | right | ~1250-1380, long narrow | both |
| Grandstand 32 | south of T6 | right | ~1210-1250 | both |
| Grandstand 31 | outside T8, between T8 and T9 | left | ~1990-2050 | both |
| Grandstand 21 | hairpin approach, T9-T10 straight, north side, stepped | left | ~2540-2650 | both |
| Grandstand 15 | beyond the hairpin apex | outside (left) | ~2670-2710 | both |
| Grandstand 34 | between the two hairpin legs, just after T11 | right (inside hairpin) | ~2770-2800, small | both |
| Grandstand 24 (Lance Stroll) | south of the return leg T11-T12, long | left | ~2700-2830, ~200 m | both |
| Grandstand 47 | T12, north side | right | ~2840-2880 | both |
| Grandstand 46 | T12-Casino Straight start, south side | left | ~2850-2900 | both |
| Pit building roof screens (3) | on the paddock building facing GS1 | left | ~4200-4330 | dossier; FIA pit drawing photos show the multi-storey building |
| Pont de la Concorde | road bridge, T7-T8 | over | 1920.5 | dossier, layout crossing |

The FIA PDFs `fia-2026-circuit-map.pdf` was not rendered (2024 version read instead).

## Kit gaps
- `building/expo_pavilion_casino` — stands in for the Casino de Montreal (Expo 67 French/Quebec pavilions).
- `building/geodesic_dome` — the Biosphere (across the channel; skyline-distance prop).
- Multi-storey pit building with roof terrace and roof screens (2019 paddock building) — shared with Sochi, Hockenheim, Brands.

## Rule gaps
- Mapped OSM walls never yield concrete (0 concrete laid at all four audited tracks); no park/semi-street CircuitStyle with concrete + catch fence.
- Crossing kind `road` -> `tyre_bridge` default (Pont de la Concorde).
- No MANUAL corner-name table: circuits whose OSM has no named corners get no corner signs and thin marshal posts.
- Pit complex drops the real pit building and generates single-storey garages only.
## S/F dossier pass

In the tables: Grandstand 1 re-laid from both oversteer48 maps as two runs
on the right, 4140-4343 (the tall block from the line back about 95 m, then
the lower steps), where the old entry ran across the line to 30 m; the
unsourced "Grandstand 2" replaced by the covered Platine stand (95-165
right, 25 m back); three screens for the paddock building's roof screens,
behind the garage zone at 4220, 4270 and 4320 left, 50 m out (their
landmarks drop the dossier's "Paddocks" outline, which the pit complex
replaces anyway).

Left:

- Grandstand 17, "next to Grandstand 1", is on neither map.
- The paddock building's upper storeys and roof are the pit complex's (kit
  gap 2); the screens stand on the ground behind it.
- The screens take the road's heading, not one toward Grandstand 1.

## Landmark dossier pass

In the tables:

- `MANUAL_LANDMARKS`: Casino de Montreal as `expo_pavilion` on OSM's unnamed 164 x 126 m outline at 3718.1 right (8 m off its centre, so the 113 x 49 m annex stays); the Biosphere as `geodesic_dome`, 2696 left, 281 m off (Wikipedia coordinate). The control-tower stand-in at 2350 is gone.

Left, or for later columns:

- The Biosphere's point is from its published coordinate: check it on the trace page.
- The Concorde bridge is a road crossing: a concrete overpass by R4.

## Stands dossier pass

In the tables:

- The hairpin group moved about 600 m to 2540-3085 with its sides corrected (both oversteer48 maps); 12 and 11 behind the pit-exit road; 10 and 32 added, 22 gone (on neither map); four general-admission viewpoints as banks.
- `DROP_STRUCTURES`: the unnamed outline under Grandstand 24 (the stand itself).

## Trees dossier pass

In the tables and rules:

- `WOOD_LEAF` `*` broadleaved; `MANUAL_WOODS`: park belts outside T3-T7, T7 to the Concorde bridge and both sides of the T9-T10 straight.

Left:

- The west shoreline is not in the layout, so the belts stop at 80 m out.
