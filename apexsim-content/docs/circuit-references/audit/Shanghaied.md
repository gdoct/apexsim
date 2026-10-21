# Shanghaied audit (Shanghai International Circuit)

Lap 5432 m, clockwise. The YAML and the dossier have **no named corners** (Shanghai's are
numbered only). Stations below come from the dossier's own stands: A 20-330 (S/F,
left), B1-B3 430-740 (T1, outside), unnamed OSM stand 3250-3470 (T11-T13 = Grandstand
E), H 4500-4810 (T14 approach), K 4790-5100 (T14 exit). Pit lane right, 708 m.
Note: the layout files Grandstand A as `side: right` (its 140 m deep OSM box is centred
almost on the road), but the laid props sit about 13 m south of the centreline, i.e.
on the left, which is correct.

Maps read: `shanghai-gov-2026-grandstand-locations.png` (official 2026 map, best),
`gpdestinations-grandstand-map.jpg` (section plan of A/H/K, start line, podium area).

## Cells

| Column | Cell | Reason |
|---|---|---|
| S/F | dossier | Grandstand A, 9 roofed runs along the straight on the left (explain `stand A看台: 9 x bay_10m_large_roof`), and the generated pit complex on the right (the OSM `garages` block is skipped as "in the pit complex") match the dossier "Start/finish". Missing: the race control tower next to the podium building (dossier "Buildings": control tower, Podium Building), which can be a `tower` MANUAL_LANDMARK (`control_tower`), and the live screens. The F1 Fanzone and Fountain Plaza behind A (gov map) are not there either. |
| Landmark | kit | The two wing-shaped sky bridges are MANUAL_CROSSINGS `arch`, which falls through to `tyre_bridge` (explain `bridge Press Centre Wing / Sky Restaurant Wing: tyre_bridge`). Their own comment admits the shape is not represented. The lotus-leaf A roof and the lake pavilions are also kit-less, but they are secondary. |
| Stands | dossier | A, B1-3, E (unnamed), H and K are all laid. Missing: the GA grass banks C, F, J and L (gov map, dossier table), which want `spectator_bank_10m` or `banking_seats` runs in MANUAL_STANDS. E has no name, so it is stamped `stand (unnamed)`: give it its name. Rule note: the depth >= 25 m roof rule roofs B1-B3, which the dossier says are uncovered (built 2025); H and K being covered is right. |
| Trees | dossier | The dossier has 0 woods (`woods Counter()`). 13 `natural=tree_row` and 3 `leisure=garden` sit unclassified, yet groom still lays 6500 trees including conifer_m 291 and poplar 301. The real site is flat reclaimed land with lawns, ornamental broadleaf rows along the access roads and water channels. Wanted: tree rows extracted as broadleaved belts, and the conifer share dropped. |
| Barriers | ok | Only 15 mapped fences. Geometry gives armco 1459, tecpro 1379, `armco_4m_fence` 373 and tyres 225 (explain). That is plausible for Tilke run-off with Tecpro and fencing. Not seen on screen. |
| Boards | dossier | 501 hoardings, 20 light panels and the big car parks (2094 props, gov map P1-P17) fit. The FIA 2024 circuit map (`fia-2024-chinese-gp-circuit-map-pit-lane.p2`) shows ~35 marshal posts (M1.3 to M16.2) and 20 marshal lights. The 20 light panels match, but only 12 marshal posts are laid, because the dossier has no corners and posts fall back to every 400 m. Fix: author the numbered corners T1-T16 (p2 gives every corner's place) as dossier corners, which also gives marshal posts and corner signs per corner. |
| People | kit | GA grass stands C, F, J and L, the fan zone behind A and the marshal posts all need figures: crowd cards on banks and marshals. |

## Positions from the maps

| Structure | Station range | Side | Extent | Map |
|---|---|---|---|---|
| Grass Stand C (GA) | ~1000-1400 (T4-T6 run) | left (outside, north of the run) | ~250 m bank | gov-2026-grandstand-locations.png |
| Grass Stand F (GA) | ~4050-4450 (back straight, before H) | left | ~300 m bank | gov map; gpdestinations "草地 F" |
| Grass Stand J (GA) | ~4780-4860 (outside the T14 hairpin) | left/outside | ~100 m curved bank | gov map |
| GA Zone L | ~5250-5400 (between T16 and the start of A) | left | ~120 m | dossier; gpdestinations "草地 L" by K12 |
| Grandstand E (name it) | 3250-3470 (already laid) | left | 11 runs | gov map "Grandstand E" |
| Sky bridge | ~330-350 (T1 end of A, by Grandstand Club) | over the track | one span | gov map "SKY BRIDGE": only one is drawn in 2026; the station-15 wing is from Wikipedia |
| Podium area (颁奖区) | ~5300-5380, just upstream of the start line | right (pit side, Paddock Club) | - | gpdestinations-grandstand-map.jpg. The bake puts the podium on the middle garage instead, so it sits about 150 m off. |
| Control tower + podium building | ~0-60 | right, behind the pits | tower about 9 storeys | dossier; not drawn on the maps |
| F1 Fanzone (merch/exhibition) | 20-330 | left, behind A | strip ~300 x 30 m | gov map (orange hatching) |
| Fountain Plaza + metro station | ~100-250 | left, ~250 m behind A | round plaza | gov map |
| Live screens | ~600 (T1), ~3300 (E), ~4500 and ~4650 (back straight/H), ~5000 (K) | various | 6 screens | gov map |

From the FIA 2024 pit lane drawing and red zone (`fia-2024-chinese-gp-circuit-map-pit-lane.p3`, `.p4`):
- **The pit building has 33 numbered garages plus FIA bays A-C**, 36 units in one straight row with stair cores every ~4 garages. The bake's `PIT_GRID_BOXES` minimum is 24, so check the generated box count against 33.
- **A long separate pit entry road** branches right before T16 (p2/p3: "Pit Lane Starts" well after the SC1 line). The pit exit runs alongside the track past the SC2 line after T1 entry. The OSM-derived lane (708 m) should be checked against this.
- Pole is on the LHS.
- A round yellow structure stands at the T1 end of the garage row (p4, top), probably the tower/rotunda base by the sky bridge.
- The FIA p2 corner anchors, in layout terms: T1/T2 snail ~650-900, T6 hairpin west end, T11-T13 ~3250-3500 (matches stand E), T14 hairpin ~4800, T16 ~5350, control line ~0.

## Kit gaps

- `bridge/wing_skybridge`: Shanghai's cantilevered wing platforms over the S/F straight.
- Optionally `grandstand` with a lotus-leaf roof for A.
- People family.

## Rule gaps

- The same roof rule as at RedPullRing: B1-B3 are open stands, but at 33-46 m deep they get roofs.
- Trees fall back to a generic broadleaf/conifer/poplar mix when a dossier has no woods. Shanghai's flora should come from the circuit style, broadleaf only (check `circuit_style` flora for Shanghai).
## S/F dossier pass

In the tables:

- `STAND_COVER`: A看台 covered.

Left, or for later columns:

- The layout files A看台 as side "right" (its 140 m box is centred nearly on the road) but its outline and laid runs are on the left (front lat +17 m), which is correct; the field is cosmetic.
- Race control tower + podium building (dossier "Start/finish": next to each other, pit side): not drawn on any map here, so no `tower` landmark is placed. The gpdestinations map puts the podium area (颁奖区) at ~5300-5380 right, by the Paddock Club; the bake lays the podium on the middle garage (~150 m off) - a bake/pit-complex matter.
- The 2026 map draws one SKY BRIDGE only, at the T1 end of A (~330-350); MANUAL_CROSSINGS also has a "Press Centre Wing" at st 15 (from Wikipedia). Landmark column: check it against the 2026 map.
- GA Zone L (dossier, gpdestinations "草地 L" by K12, between T16 and A) is not on the 2026 map, which draws the "T16 Club" hospitality building there instead: left out. T16 Club and "Grandstand Club" (T1 end of A) are hospitality buildings, not stands.
- F1 Fanzone (orange hatching behind A) and Fountain Plaza are crowd areas behind the stand; no kit piece or table entry carries them.
