# Shebang audit (Sepang International Circuit)

Lap 5533 m, clockwise. There are no named corners in the YAML. The stations below come
from centreline sampling: main straight 0-600 running west; T1/T2 west hairpin ~620-800;
north-west climb ~800-1150; top run east ~1200-1550; T5/T6 ~1600-1900; ~2000-2500 to
the east end (F); T7/T8 ~2550-2650; westward run ~2700-3100 just south of the back
straight; T9 ~3150-3250; T10/T11 ~3300-3550 (south); T12/T13 ~3600-4000; T14 west
hairpin ~4100-4250; back straight 4300-5100 running east; T15 left hairpin ~5100-5250.
Pit lane right (north), 1013 m.

Maps read: `gpd-grandstand-map-2026.jpg` (the general map, best), `says-grandstand-map-2026.png`.

## Cells

| Column | Cell | Reason |
|---|---|---|
| S/F | dossier | **Main Grandstand north face is lost.** explain: `skipped stand Main Grandstand: nothing to build clear of the pit lane`. MANUAL_STANDS has both faces as `side="right"`, but the dossier ("Start/finish", "Modelling pitfalls": the stand sits *between* the straights) and both maps put it left of the main straight, opposite the pits. The other face (4300-4950) is also `right`, and its 17 runs are laid ~17 m **south** of the back straight, on the South Paddock side, outside the V (checked prop by prop against the centreline). Both entries need `side="left"`. Pit complex north is correct. The podium above the pit lane comes from the bake. |
| Landmark | kit | Sepang's signature is the 1.3 km V-shaped double-faced grandstand with palm-frond canopies and the Tower (Hibiscus) stand at its point (dossier "Buildings"). No kit piece has that roof. 6 of 7 authored floodlights are laid; one is skipped in the pit zone. That is fine. |
| Stands | dossier | K1, F and Turn 4 are laid (OSM K1/F; Turn 4 is a guess). Missing: Tower grandstand, and the hillstands B, C1, C2, C3, D, E, G1, G2 and G3 (all on gpd-grandstand-map). K2 sits at 650-850, overlapping K1. The map has it north of K1, at ~900-1150. Neither map shows a "Turn 4 Grandstand" at 1550-1720: its own comment calls it approximate, so recheck or drop it. Rule: the roof rule roofs K1 and F. The dossier says K1 is covered (fine). F is "elevated", roof unconfirmed. |
| Trees | dossier | groom laid **0 trees** (`groom tree: 2409 grass_clump, 2465 wildflower_clump` only). The dossier has 2 woods (1 mixed, 1 broadleaved) and 161 far impostors. Real Sepang has 5000+ planted palms and oil-palm country around (dossier "Palm trees"; the darker green belts on the gpd map). Wanted: MANUAL_WOODS oil-palm belts (Tropical flora gives palms). |
| Barriers | ok | 30 walls, 16 fences and 1 guard rail are mapped. Geometry gives armco 1544, tecpro 1459, `armco_4m_fence` 398 and tyres 124. Plausible for a Tilke track with wide run-off. Not seen on screen. |
| Boards | ok | 622 hoardings, 12 marshal posts, 20 light panels, named car parks (A1, Bay 1, Bay 12, F2, F3, G2) and food stalls. There are no corner signs because the corners are unnamed, which is correct. The Welcome Centre is laid as 2 `media_centre`. |
| People | kit | Large GA hillstands (B, C1-3, D, E, G1-3, K2), the mall/fan zone between the wings and the marshal posts need figures: crowd cards on banks and marshals. |

## Positions from the maps

| Structure | Station range | Side | Extent | Map |
|---|---|---|---|---|
| Main Grandstand north face | 20-560 (plus ~5250-5530 towards T15) | **left** (south of the main straight) | 1.3 km total V, 2 tiers, covered | gpd-grandstand-map, says-grandstand-map |
| Main Grandstand south face | 4300-4950 (to ~5100) | **left** (north of the back straight) | as above | same |
| Tower grandstand | ~5130-5230 (T15) | left (inside the T15 left hairpin, at the V point) | ~40 m, 4 sections | gpd map (red dot at the V), dossier |
| K2 hillstand (GA) | ~900-1150 | left (west/outside) | ~250 m bank | gpd map, says map "K2 General Admission" |
| G1 hillstand | ~1650-1800 | left (east of the southward T5 run) | ~150 m | gpd map |
| G2, G3 hillstands | ~1850-2050 | left | ~100 m each | gpd map |
| E hillstand | ~2580-2650 | left (outside the east tip, south of F) | ~80 m | gpd map |
| D hillstand | ~2700-2800 | left (south of the westward run) | ~100 m | gpd map |
| C3 hillstand | ~3150-3250 | right (outside T9) | ~80 m | gpd map |
| C2 hillstand (GA, partly covered) | ~3250-3550 | left (east/south of the T10-T11 descent) | ~300 m long bank | gpd map, says map "C2 General Admission" |
| C1 hillstand | ~3500-3600 | left (south, bottom of the lap) | ~80 m | gpd map |
| B hillstand | ~3650-4150 | left (south-west, outside T12-T14) | ~450 m bank | gpd map |
| South Paddock | ~4450-4800 | right (south of the back straight) | ~250 x 100 m | gpd map. This is where the mis-sided Main Grandstand runs land today. |
| Mall area / fan zone + Welcome Centre | ~560-650 / ~4300 | between the wings (west mouth of the V) | ~200 x 60 m | gpd map |

## Kit gaps

- `grandstand/canopy_frond_10m` (or a roof variant): the palm-frond canopy of the Main and Tower grandstands.
- People family.

## Rule gaps

- The stand roof-by-depth rule, as at RedPullRing (F at 32 m deep gets a roof without evidence).
- None specific otherwise. The S/F loss is a dossier side error, not a rule fault. A guard that flags a stand whose front lands closer than its depth to *another* stand's back, or on the far side of a leg, would have caught it.
