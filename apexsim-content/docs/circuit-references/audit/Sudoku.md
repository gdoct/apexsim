# Sudoku audit (Suzuka)

Sources read: `circuit-references/Sudoku.md`, README row ("real bridge at the crossover",
"start line ~300 m ahead of finish"), maps `suzuka-official-seat-map.png`,
`suzuka-grandstand-pit-building.pdf` (as `...pdf p.1`), the `ats-dress --explain` output,
`Sudoku.layout.json`, `MANUAL_STANDS["Sudoku"]` (only M), `.ats` by stamp. Not read: the FIA
media kit PDF and the F1 webp (the seat map and the pit-building drawing cover the S/F).

Lap 5800 m. Corners (YAML, display names): Turn 1 702, Turn 2 852, Esses 1116, Reverse Bank
1559, Hippo (Dunlop) 1758, Dagger 1 2297, Dagger 2 2458, Hairpin 2922, Back Straight 4308,
130R 5046, chicane 5388/5445, Main Straight 5730. Pit lane right, entry ~5520, exit ~620.
Crossover: 0 bridges from the dress pass; the deck and walls come from the export (as built
for the underpass), not this column's concern.

## Cells

| Column | Cell | Reason |
|---|---|---|
| S/F | dossier | Pit building is the export's (explain: "skipped building ピットビル: in the pit complex"); V1, V2, VIP laid from OSM on the left. Missing (pit-building drawing): the three Giant Screens on the pit building's roof, the two signboard bridges across the straight (one at the T1 end carrying the Signal = start lights, one at the last-corner end), the flag towers at both ends, stand A2. The podium is at the **last-corner end** of the pit building next to the Media Center, while the export puts it on the middle garage's roof (rule). V2's roof (sections 10-12) not modelled: V2 laid `bay_10m_large` unroofed. |
| Landmark | ok | The Ferris wheel is laid (`landmark サーキットホイール: ferris_wheel`, st 5731 L, behind the main grandstand at the final-curve end, as the seat map's "Ferris Wheel Lounge" and the dossier say); the crossover bridge is the export's. Optional extra: the paddock's "Leader Tower" (seat map, in the infield behind the pits) as `tower`. |
| Stands | rule | All lettered stands but A2 and H come from OSM. The 25 m always-roofed rule roofs open stands: C (uncovered per dossier, depth 30), D1-D4 (wooden bleachers, OSM depth 73), E (depth 75), O (27), R are laid `bay_10m_large_roof`; Q1/Q2 (elevated and covered) are laid open. Dossier: A2 and H missing; the OSM pair named "G" at st 2639-2645 R (on the 110R leg under the crossover) is where the seat map has **H**; real G (130R) is the pair at 5038-5044 L. West-area GA banks (J, L, N, Degner viewing area) have no family. |
| Trees | rule | 29 woods, all `mixed`, many 100-400 m from the road (58,214 m^2 at 5530 L 208 m out, 52,098 at 2394 L 158 m, 28,389 at 4386 R 112 m); groom only plants 22-90 m from the road and laid ~150 trees (plus 4,317 grass/wildflower clumps); `surroundings::forests` lays impostors only on DEM slopes rising 25 m+ and laid none here. The mapped woods beyond the belt stay bare. |
| Barriers | ok | 1293 armco, 767 armco_fence, 1664 tecpro, 61 tyres: Tecpro on the fast bends, fences at the stands. Gravel at T2/T7/T9/T14/T17 is a surface matter. |
| Boards | ok | 631 hoardings, 13 distance boards (5 braking corners), 12 corner signs (Latin display names), 19 marshal posts, 25 light panels. |
| People | kit | West Area GA banks (hairpin to 130R), Degner viewing area, the crowd in GP Square and the fan zone behind V/A, marshals. No figure kit. |

## Positions from the maps

- **Giant Screens on the pit building** (`suzuka-grandstand-pit-building.pdf`): three, on the roof facing V1/V2, at about 25 %, 50 % and 75 % along the 357 m building (OSM building st ~5747 -> 304): ~st 35, 125, 215 R, roof height. Landmark `screen` (needs a roof-mounted z; see rule gaps).
- **Signboard bridge + Signal (start lights)**: across the straight at the First Corner end of the pit building, ~st 300 (the start line is ~300 m ahead of the finish). `bridge/start_gantry` / `sign_gantry`.
- **Signboard bridge (finish end)**: across the straight at the last-corner end, ~st 5760. `bridge/sign_gantry` or `banner_gantry`.
- **Flag towers**: one by each bridge on the pit side, ~st 300 R and ~5760 R. `sign/flag_pole` (tall).
- **Podium + Media Center**: last-corner end of the pit building, upper floor, ~st 5770-5800 R.
- **A2**: left of the straight's end into T1, after A1, ~st 520-640 L, depth ~15. (OSM has a long thin unnamed structure at 492 L, 264 x 8 m, 5.6 m off the road: may be A2's outline tagged as a building; check.)
- **H**: on the 110R leg next to the crossover, right side, ~st 2600-2700 R = the OSM "G" pair; rename to H.
- **I**: hairpin exit, right, the unnamed OSM stands at 2795-3082 R. ok.
- **J** (West Area GA): inside of the run from the hairpin to Spoon, ~st 3150-3450 L, a bank.
- **L** (West Area GA): inside Spoon, ~st 3550-3750 L.
- **N** (West Area GA): outside Spoon's second apex, beyond M, ~st 3760-3850 R.
- **Degner viewing area** (West Area): outside Degner 1, ~st 2280-2330 L (outside of a right-hander).
- **Circuit-vision screens** (yellow bars on the seat map, ~20): along the main straight (inside), T1-T2 inside, the Esses, Dunlop, Degner, the hairpin, Spoon, 130R, the chicane; the dossier names those opposite C and at D blocks 4-5 as the giant ones.
- **Leader Tower**: infield behind the pit building, between the paddock and the T3-T5 Esses, ~st 1000-1100 R, 150 m in.

## Kit gaps

- People figures (GA banks, GP Square crowd).
- None structural: gantries, flag poles, screens and the wheel exist.

## Rule gaps

- Always-roofed at 25 m depth (`ASSUMED_ROOF_DEPTH_M`) roofs C, D, E, O, R; OSM stands cannot be marked covered/open from the dossier (Q1/Q2 covered, V2 part-roofed).
- Podium is always on the middle garage's roof in the export; Suzuka's is at the last-corner end.
- Roof-mounted landmarks (screens on the pit building) have no height override.
- Mapped woods beyond the 90 m groom belt get no trees unless a DEM slope rises 25 m (`forests`): Suzuka's flat-ish woods stay bare.
- No bank family for GA areas (West Area).
## S/F dossier pass

In the tables:

- `MANUAL_STANDS`: A2 (505-620 left).
- `MANUAL_LANDMARKS`: screen at 450 right.

Left, or for later columns:

- Side effect of the screen: it drops the unnamed OSM building at 492 L (264 x 8 m, 5.6 m off the road) from the dossier's structures, since drop_structures_under_landmarks matches within half its length on either side. The dress already skipped that building ("in the pit complex"), so nothing changes on screen; leave the screen out if the dossier must stay byte-identical there.
- The three Giant Screens stand on the pit building's roof (suzuka-grandstand-pit-building.pdf, ~st 35, 125, 215 R): not placed; a ground screen there lands in the pit complex. Rule gap: roof-mounted landmark height.
- The two signboard bridges (Signal / start lights at the First Corner end ~st 300, a second at the last-corner end ~5760) and the flag towers beside them on the pit side are crossings / furniture, not these tables (MANUAL_CROSSINGS).
- Podium at the last-corner end of the pit building beside the Media Center: the export puts it mid-building (rule gap).
- S (OSM, 5698 L) is the stand before V1 at the straight's start: laid; R and the Q stands are the final chicane/curve (Stands column).

## Landmark dossier pass

Nothing went in.

- Nothing needed: the OSM Ferris wheel is laid. The Leader Tower is a scoring pylon, not race control: no piece.

## Stands dossier pass

In the tables:

- Roofs: C, D1-D4, E, O and R open, Q1 and Q2 covered; D1-D4 `banking_seats`; L moved behind the fence where the old "M" span was, M to the Spoon exit; N added; banks for J and the Degner viewing area.

Left, or for later columns:

- The OSM "G" pair on 110R is H by name only.
- An unnamed OSM building behind L.

## Trees dossier pass

In the tables and rules:

- Mapped woods past the belt fill with forest impostors from 120 m out, flat ground or not (R8).
