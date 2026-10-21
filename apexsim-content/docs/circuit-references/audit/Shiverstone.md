# Shiverstone audit (Silverstone)

Lap 5884 m, clockwise. YAML/dossier corners: Hamilton Straight 136, Abbey 426, Farm 635,
Village 895, The Loop 1055, Aintree 1238, Wellington Straight 1568, Brooklands 1941,
Luffield 2206, Woodcote 2548, Copse 3104, Maggotts 3690, Becketts 3876, Chapel 4025,
Hangar Straight 4826, Stowe 5065, Vale 5281, Club 5683. Pit lane right (inside, the
Wing), 1129 m.

Maps read: `2026-british-gp-spectator-map.pdf` rendered the best map;
north points left on it). The others were not needed.

## Cells

| Column | Cell | Reason |
|---|---|---|
| S/F | dossier | The pit lane is on the right with the bake's garages (explain `skipped building Silverstone Wing: in the pit complex`). Hamilton Straight A is laid as 3 roofed runs, but only 95 m at station 11. The map has Hamilton Straight **A and B**, Club Corner and Club A along the outside of the Club-Hamilton-Abbey run. B and Club Corner are missing. Club A/B are in the dossier only as buildings (see Stands). The F1 Paddock Club is generated with the pits. |
| Landmark | kit | The Wing (390 m, 3 storeys, the wing-shaped roof with media centre and race control; dossier "Start/finish") is the venue's signature. It is dropped for the bake's plain `garage_6m` row, and no kit piece has its roof. Also missing, and a dossier fix: the giant wheel (map "Giant Wheel" icon in the infield by Luffield/Brooklands) can be a `big_wheel` MANUAL_LANDMARK (`ferris_wheel` exists). The main stage (`stage` -> `fanzone_stage`) is missing too. The 13 OSM floodlights by the BRDC/Luffield complex are laid as `floodlight_tower`, which is fine. |
| Stands | dossier | 17 OSM stands are laid. Five real grandstands OSM tags as buildings are laid as **media_centre blocks**: `building Stowe A` (3), `Stowe C` (2), `Vale` (4), `Club A` (1), `Club B` (4) (explain). `Village A` (2) is a stand too. They should be promoted to MANUAL_STANDS, as Barcelona's Tribuna F is. Missing altogether: Landostand (Stowe), Lakeside, Hanger, Stirling A/B, Copse D, Woodcote C, Hamilton Straight B, Club Corner and Luffield Corner. `Silverstone Six` (3597 right) is unconfirmed as a stand. |
| Trees | rule | The dossier maps 20 woods (all `mixed`), but groom planted only ~16 trees (`groom tree: 4 broadleaf_l, 2 broadleaf_m, ... 6 poplar`). There are no far-forest impostors because the site is flat: `IMPOSTOR_MIN_RISE_M` 25 m and "forest impostors on far slopes". The woods lie beyond the 22-90 m belt, so a flat circuit's mapped woods vanish entirely. Silverstone's tree belts (Copse, Becketts/Chapel, along Hangar and Stowe, the Whittlebury woods) should show. The `mixed` leaf type is fine. |
| Barriers | ok | 49 walls, 88 fences and 3 guard rails are mapped. Groom gives armco 1381, `armco_4m_fence` 892 (many stands and GA), tecpro 1209 and only 9 tyre modules. That is plausible: Silverstone runs Tecpro and armco with fencing. Not seen on screen. |
| Boards | dossier | 528 hoardings, 17 corner signs, 17 marshal posts and big car parks match. **No campsites are laid**, but the 2026 map has 7 campsites (Silverstone Woodlands 3, Dadford Road 4, Golf Club 5, Woodcote Fields 6, Abbey Meadows 7) and two glamping areas (Nova 1 in the Hangar-Stowe infield, The Ridings 2 outside Stowe). Also missing: the F1 Fanzone, the hospitality villages (47-64) and the heliport. |
| People | kit | Huge GA areas (the blue strips on the map), packed stands, the fan zone and marshal posts need crowd cards on banks and marshal figures. |

## Positions from the maps (all from 2026-british-gp-spectator-map.pdf)

| Structure | Station range | Side | Extent | Note |
|---|---|---|---|---|
| Hamilton Straight B | ~100-250 | left (outside, opposite the Wing) | ~120 m | Between Hamilton A (Club end) and Abbey A |
| Hamilton Straight A (extend) | ~5780-5884 / 0-60 | left | today only -37 to 58 | |
| Club Corner stand | ~5620-5720 | left (outside Club) | ~120 m curved | The OSM building "Club B" (5724 left, 170 m) is probably this one |
| Club A | ~5520-5620 | left | ~60 m | OSM building "Club A" (5616 left) |
| Vale | ~5300-5450 | left (outside Vale) | ~150 m | OSM building "Vale" (5415 left, 150 m) |
| Landostand (GA Plus, curved) | ~4950-5150 | left (outside Stowe) | ~250 m | OSM buildings "Stowe A" (4997) and "Stowe C" (5097) |
| Lakeside | ~5150-5250 | right (inside, facing the Stowe-Vale run) | ~100 m | |
| Hanger | ~4250-4400 | right (inside Hangar Straight, after Chapel) | ~80 m | |
| GA strip Becketts-Chapel-Hangar | ~3850-4750 | left (outside) | ~900 m | Blue on the map |
| GA strip Stowe-Vale | ~5150-5300 | right | ~150 m | |
| GA Brooklands (65) | ~1950-2150 | right (inside Brooklands/Luffield) | ~200 m | |
| GA Farm Curve | ~650-800 | right (inside) | ~150 m | |
| Stirling A/B | ~2850-3050 | left (outside National Pits Straight, opposite the old pits) | 2 x ~80 m | |
| Woodcote C | ~2600-2650 | left | ~50 m | |
| Copse D | ~3150-3200 | left (outside Copse) | ~50 m | |
| Luffield Corner (43) | ~2150-2250 | left | ~80 m | |
| Giant wheel | ~2000-2100 | right (infield, ~80-120 m from the road) | one wheel | |
| F1 Fanzone (hatched) | ~1400-1700 | right (infield between Wellington Straight and Village/Brooklands) | ~200 x 120 m | |
| Main Stage | ~1750 | right (infield) | stage | |
| Heliport | ~4550-4650 | right (inside Hangar Straight) | pad | |
| Campsites 3-7, glamping 1-2 | 3: outside Hangar/Stowe (east); 4/5: beyond Dadford Road behind Club/Vale; 6: outside Woodcote; 7: beyond Abbey; glamping 1: Hangar-Stowe infield (~4700-4900 right, ~250 m in); 2: outside Stowe | various | large | |

## Kit gaps

- `pit/wing_pit_building` (or a `building` roofdeck variant): the Wing's 390 m three-storey pit building with its wing roof, laid by the bake instead of plain garages, as `pit_building_roofdeck` is for street circuits.
- People family.

## Rule gaps

- Far woods on a flat site: `dress::surroundings` forest impostors need `IMPOSTOR_MIN_RISE_M`, so a flat circuit's dossier woods beyond the groom belt are not drawn at all. Wanted: impostors for mapped woods regardless of rise, or a wider belt.
- An OSM building whose name matches a grandstand (`Stowe A`, `Vale`, `Club A/B`, `Village A`) is laid as a `media_centre`. A name heuristic in `osm_layout.py` (or a dossier promotion) would fix it at every circuit, although strictly that is a dossier-tool change.
## S/F dossier pass

In the tables:

- `MANUAL_STANDS`: Club A (5614-5634 left); Club B (5643-5766 left); Hamilton Straight B (100-190 left); Abbey A (200-272 left).

Left, or for later columns:

- Hamilton Straight A stays the OSM outline (-46..49). The map draws it nearer Club (~-120..0), but the map is schematic there (Club's exit bend) and its length agrees with OSM's 95 m; the audit's "extend to 5780" is not supported by OSM and is left out.
- Hamilton B's span is the least certain (map scale +-50 m); its gap is set by the hotel behind it, not by a source.
- The Wing (kit gap) and the F1 Paddock Club are the pit complex's. Vale, Stowe A/C and Village A (also plain OSM buildings) are the Stands column's.
