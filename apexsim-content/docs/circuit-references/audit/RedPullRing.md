# RedPullRing audit (Red Bull Ring, Spielberg)

Lap 4308 m, clockwise. YAML/dossier corners: Niki Lauda (T1) 449, Remus (T3) 1388,
Schlossgold (T4 "Rauch Turn" on the 2026 maps) 2206, Rauch (T5/T6) 2721,
Wuerth (T7 Graz Turn) 3031, Rindt (T9) 3775, Red Bull Mobile (T10) 4002. T8 is
unnamed (~3300-3450). Pit lane right (inside), 1000 m.

Maps read: `redbullring-2026-spectator-map.png`, `redbullring-2026-wayfinding.pdf`
(page 1, rendered `redbullring-fia-media-kit-2026.pdf` p3 (aerial
photo only). `redbullring-lageplan-2021.jpg` is **not an image**: it is a saved
HTML page (failed download) and needs re-fetching.

Media kit follow-up: rendered PDF pages p1-p6 are the cover, contents, circuit details
with an aerial photo, Murtal/TAUROA hotels, and timetables. None of them is a site plan.
The contents list a **"Site Map" on printed page 14, which is PDF page 8**. It is read below
("Venue plan").
- p4 adds two facts. The region's forests are Swiss stone pine ("Zirbe"), which supports `needleleaved` for the hill woods. The **Schönberghof hotel** is a long multi-storey white block on the hillside "directly at the Red Bull Ring" with 80% of rooms overlooking the track. Its position is unconfirmed; the wayfinding map puts "Schönberg" by T3-T4, outside, behind the Schönberg stand. A `hospitality_3f` or `skyline_lowrise` stand-in fits.

## Cells

| Column | Cell | Reason |
|---|---|---|
| S/F | dossier | Pit complex right and Start/Ziel 418 m covered stand left (explain: `stand Start/Ziel Tribuene: 18 x bay_10m_large_roof`) match the dossier "Start/finish" section. Missing: the giant screen on the pit-building roof and the S/F live screens (wayfinding: 3 live-screen icons on the straight, spectator map: 3 TV icons) - no `screen` landmark exists; the media centre Wing (see Landmark) and the F1 Fanzone stage outside T1 (wayfinding "F1 Fanzone", stage icon) are absent. |
| Landmark | kit | The bull (`landmark Der Bulle vom Spielberg: bull_statue`, OSM artwork at station 3161 left, next to Tribuene Mitte) is right - the dossier table's "hilltop near the Red Bull grandstand" is wrong, osm_layout's MANUAL_LANDMARKS note (next to Mitte) and OSM agree. The voestalpine Wing (92 x 20 m steel wing-shaped media centre over/behind Start/Ziel, dossier "Start/finish") has no kit piece; the advertising bridge T1-T2 is laid as `bridge T1 climb footbridge: truss_bridge` at 600 (MANUAL_CROSSINGS) - acceptable stand-in, `banner_gantry` would read closer. |
| Stands | dossier | 9 OSM stands laid (8 stamps; Red Bull is two runs). Missing Schoenberg stand (spectator map "Grandstand Schoenberg", wayfinding "Schoenberg") and T8 stand (both maps). Also a **rule** issue: `ASSUMED_ROOF_DEPTH_M` 25 m roofs every deep OSM stand (explain: Steiermark, T3/"Tribuene 13", Mitte, Ost, T10, 7 of Red Bull's bays all `_large_roof`) while the 2026 spectator map marks only Start Ziel and North as "(COVERED)". |
| Trees | ok | 8 dossier woods, all `mixed`; far forest 4444 conifer + 1504 broadleaf impostors matches the spruce-covered Mur-valley hills in the media-kit aerial (p3). The infield and hillsides are open meadow, which the sparse near trees (31 broadleaf_l, 17 conifer_m, 43 poplar) respect. Optional: mark the big hill woods `needleleaved` (media kit p4: stone-pine forests). |
| Barriers | ok | 77 mapped barriers (tyres/walls/fence) drive `barriers::decide`: 1297 armco, 611 tecpro, 387 tyre modules, 157 `armco_4m_fence` in front of stands. Matches dossier/FIA notes (tyre rows T2/T4, fences T1-T2). Not judged on screen. |
| Boards | dossier | Hoardings 387, 7 corner signs, 11 marshal posts, 19 light panels, car parks 291 props: fine. Camp sites are near-empty (`surroundings: camp sites: 8 prop(s)`, one OSM `Camping Rot` POI) while the venue has large colour-coded campsites (dossier "Car parks / campsites", wayfinding). Steiermark Village (fan village behind Start/Ziel at the T10 end, wayfinding) absent. |
| People | kit | Venue has GA grass banks (Schoenberg green zone T3-T4 outside, blue GA strip inside the T3-T4 run, unofficial GA outside T10), packed hillside Red Bull stand, marshal posts. Wanted: seated/standing crowd cards for GA banks, marshal figures at posts, fan-zone crowd. |

## Positions from the maps

| Structure | Station range | Side | Extent | Map |
|---|---|---|---|---|
| Grandstand Schoenberg (open, GA "green tickets" bank around it) | ~1750-2100 (T3-T4 run, just before Schlossgold 2206) | left (outside, north of the top straight) | ~150 m, bank behind ~250 m long | spectator-map.png, wayfinding.pdf |
| GA blue strip (general admission) | ~1500-1950 | right (inside of the T3-T4 run) | ~300 m strip, ~15 m | wayfinding.pdf |
| Grandstand T8 (4 sectors A-D) | ~3250-3400 (approach to T8) | left (outside of the right-hand T8, in the circuit infield) | ~80 m | spectator-map.png, wayfinding.pdf |
| Pit-roof giant screen | ~150 (mid pit building) | right, on the pit roof | one screen | dossier "Pit building"; not drawn on maps |
| Live screens | S/F ~100, ~250, ~4100; T1 exit ~560; T2 ~900; T3 ~1350; top straight ~1600, ~1800; T4 ~2150; T7 ~3050; T8 ~3350; T10 ~3950 | mostly outside | 12 screens | spectator-map.png (TV icons), wayfinding.pdf |
| voestalpine Wing (media centre) | ~150-260 | left, behind/above Start/Ziel stand | 92 x 20 m, elevated | wayfinding.pdf (purple hatched "Media Centre" at the T1-end third of the Start/Finish stand) |
| F1 Fanzone + stage | ~350-500 | left (outside T1, behind Steiermark) | ~120 x 60 m | wayfinding.pdf, spectator-map.png |
| Steiermark Village | ~4150-4250 / 0 | left, behind Start/Ziel at its T10 end, by Main Gate A | ~70 x 50 m | wayfinding.pdf |
| F1 Experiences Champions Club (CC) | ~4080-4180 | right (inside after T10, before pit entry) | small covered terrace building | wayfinding.pdf |
| Support event paddock | ~150-350 | right, behind the F1 paddock | ~150 x 70 m | wayfinding.pdf |
| Pedestrian bridge | ~520-560 (T1 exit, start of Red Bull stand) | over the track | - | wayfinding.pdf ")(" icon |

The Suedwest stand of the dossier table is not on either 2026 map (dropped).

## Kit gaps

- `building/wing_media_centre` (or `bridge`-style elevated wing roof): the voestalpine Wing above Start/Ziel.
- People family (crowd cards on banks, marshal figures).

## Rule gaps

- `dress::stand_family`: `ASSUMED_ROOF_DEPTH_M` roofs every stand >= 25 m deep, and OSM depth here includes the hillside bank, so open hill stands get roofs. Needs a dossier way to say "known open" (e.g. `covered: false` explicit vs unknown, or `open: true`) honoured over the depth rule.

## Venue plan (`redbullring-fia-media-kit-2026.pdf` p.8, printed p.14)

The media kit's site plan names every stand. Against the lap (clockwise from the line):

- **Tribüne Start Ziel**: outside the main straight (pit lane opposite, "Boxen | Fahrerlager"), between the T10 exit and the line. **Steiermark Tribüne** continues it along the straight towards T1, with **Steiermark Village** behind Start Ziel.
- **Red Bull Tribüne**: outside the T1 exit and the run up to T3 (left), long, on the hillside. **F1 Fan Zone** behind T1's outside, by gate C.
- **Tribüne T3**: outside T3 (the hairpin at the top).
- **Stehplatzbereich** (general-admission bank): the hillside outside the T3-T4 straight, the whole length. **Schönberghof / Speed Club** (a long multi-storey block) and **Tribüne Schönberg** stand behind it, Schönberg towards T4.
- **Tribüne Nord**: outside T4 (the T4 right-hander), by gate E.
- **Tribüne T8** and **Tribüne Mitte**: inside the infield loop, T8 on the T6-T7 side and Mitte facing T5-T6, with the bull beside Mitte.
- **Tribüne T9** and **Tribüne T10**: outside the last two corners, by gate F.
- No big screens are drawn.
