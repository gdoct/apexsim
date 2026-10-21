# Gnocchi (Sochi Autodrom, 2014-21 GP layout) — dressing audit

Sources: `circuit-references/Gnocchi.md`, README row; maps `racefans-sochi-autodrom.jpg`,
`f1-circuit-map-russia.webp`, `tilke-sochi-09.jpg` (photo of the pit straight: main grandstand,
pit building, walls), `fia-2018-media-kit.pdf` (p.4 text, p.6 media map rendered; too small to
place structures); the `ats-dress --explain` output; layout.json; MANUAL_STANDS (T2, T3, T4, T5, T10, T13,
T15), MANUAL_LANDMARKS (Olympic Cauldron).

Fit: `centerline_covered 0.352` (OSM now maps only the short layout, dossier pitfall), so the
Olympic Park loop is placed by the fit of the remaining 1.6 km; rmse 2.55 m.

Stations (layout has no named corners; from the centerline heading change): line 0, lap 5833.
T1 kink ~420 (R), T2 1170-1200 (R), T3 sweeper 1230-1860 (L), T4 2040 (R), T5 2460-2490 (R),
T6 2730-2760 (R), T7 2910 (R), T8 3207 (L), T9 3300-3330 (L), T10 3570-3600 (R), T11 ~3775,
T12 ~4015, T13 4704 (R), T14 4790-4820 (L), T15 5060-5090 (L), T16 5150-5180 (R), T17 5540-5570
(R), T18 5690-5720 (R). Pit lane right, 1072 m.

## Cells

| Column | Cell | Reason |
|---|---|---|
| S/F | rule | Main Grandstand (OSM, 372 m x 39 m, 142.8 left) laid as 10 x `bay_10m_large_roof`; the real one is a six-storey 400 m building with one cantilevered roof (photo `tilke-sochi-09.jpg`, media kit p.4); `bay_10m_stadium_roof` is the nearer kit piece but the stand family is picked by depth only. The pit building is multi-storey (glass, Paddock Club on top, media centre on 1st floor): the export lays single-storey garages and the OSM "Paddock-club F1" footprint (104 right, 315 m, 4 levels) as 8 x media_centre behind them — check it does not overlap the garage depth. |
| Landmark | kit | Laid: ferris wheel (Sochi Park, 873 left) ok; Olympic Cauldron as `control_tower` (no cauldron piece); Bolshoy Ice Dome as 5 media_centre boxes, Iceberg 4, Shayba 3, Sirius-Arena 5 (no dome/arena pieces). Fisht stadium not in the extract. Medals Plaza fountain inside T3 not laid (`misc/fountain_basin` exists: MANUAL_FURNITURE). Explain "skipped building Сириус: reaches across the road" (469 m x 195 m footprint at 5750 left). 72 OSM lighting masts laid as `floodlight_tower` landmarks for a day race (check: DAY_RACE_NO_FLOODLIGHTS or keep as park masts). |
| Stands | dossier | MANUAL has T2/T3/T4/T5/T10/T13/T15 on the outside: consistent with the corner directions. Missing: **T1 grandstand** ("Sergey Sirotkin", outside T1). T3 stand (1400-1700 right) runs over the OSM Sirius-Arena footprint (1606 right, offset 19 m, 273 x 162 m): both are laid, check overlap on the trace page. No GA area around the Bolshoy. |
| Trees | dossier | One wood (`mixed`); groom lays **0 trees** (only grass/wildflower clumps), while 2116 forest impostors fill the far ground. The photo shows rows of columnar cypresses behind the fences; the park has planted rows along its roads. Needs MANUAL_WOODS / tree rows (conifer/poplar) and a check that impostors are not planted on the coastal plain. |
| Barriers | rule | Groom: 1046 armco, 1282 armco_fence, 699 Tecpro, 236 tyres, **0 concrete**. The real circuit (photo) is concrete wall + catch fence on the park roads, Tecpro at corners. Same rule gap as Montreal. |
| Boards | dossier | 534 hoardings, 18 distance boards; **0 corner signs**, 14 marshal posts for 5.8 km: no named corners in the layout. Car parks ok (991 cars), a 23-house village block. |
| People | kit | GA around the Bolshoy Ice Dome "about 20 m from the track" (dossier), crowds on the Medals Plaza; marshals. Wanted: standing crowd, marshals. |

## Positions from the maps

The racefans/F1 maps place corners only; the media-kit map is too low-resolution for positions.

| Structure | Where | Side | Extent | Source |
|---|---|---|---|---|
| T1 grandstand | outside the T1 kink | left | ~330-480 | dossier "Buildings" (T1 "Sergey Sirotkin"); racefans map for T1 |
| Medals Plaza fountain | inside T3, the lap wraps its outer edge | left (inside) | centre of the 1230-1860 arc | dossier T3 note |
| Main Grandstand | left of the pit straight, opposite the pits, 400 m, six storeys, single big roof | left | ~-50 to 330 | photo `tilke-sochi-09.jpg`, media kit p.4 |
| Pit building | right, multi-storey glass with roof terraces | right | ~0-320 | photo |
| Bolshoy Ice Dome | encircled by the right-handers T5-T7 | right | ~2900-3200 (OSM 3088 right) | dossier, layout |

## Kit gaps
- `building/ice_dome_arena` — Bolshoy Ice Dome (and other arenas: Iceberg, Shayba, Adler).
- `landmark/olympic_cauldron` (tall twisted torch) — the cauldron; today a control_tower.
- `building/stadium_bowl` — Fisht stadium (if it comes into range).
- Six-storey grandstand building (stand front plus offices behind one roof); `bay_10m_stadium_roof` may do.

## Rule gaps
- Stand family chosen by depth only: a permanent multi-storey main stand gets the large roofed bay, not the stadium roof.
- Mapped walls never yield concrete; street-style park circuit has no concrete style.
- Crossing 4786.3 (`road`) laid as `tyre_bridge`.
- No corner-name table for circuits whose OSM names none.
