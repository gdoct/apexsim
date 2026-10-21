# Oskarsleben (Motorsport Arena Oschersleben) dressing audit

Lap 3692 m, clockwise. The pit lane is on the left, 1098 m long. The straight runs westward on the overview map (Start/Ziel arrow pointing left). Corners in the layout: Variante (T1) 411, Hotelkurve 555, Triple 1471, Schikane 2015, Zeppelin-Kurve 3334. The Hasseröder-Kurve is missing as a corner.
Maps read: mao-uebersichtskarte-2021.jpg. This is the official overview and the best map: numbered buildings, stands, spectator zones, bridges and tunnels. racingcircuits-oschersleben-2007.png and racetrack4you-oschersleben.svg are layout-only and were not needed. mao-parkplaetze.jpg was not read (car parks). The .pdf is the same overview as the jpg.
No Oskarsleben MANUAL_* entries exist.

| Column | Cell | Reason |
|---|---|---|
| S/F | dossier | The pit complex is generated (29 garages). The OSM "stands" "Boxen / VIP-Loungen" (55 L) and "Verwaltung" (3652 L) are skipped as pit-lane buildings, which is right. The Zuschauertribüne (main, 360 m, 4.6 R) is laid opposite. Missing: the Race-Tower / Presse Zentrum (map item 1), at the east (start-line) end of the pit building, before the line. The DTM fan-zone big screen in Zuschauerbereich Mitte behind the main stand is also missing. Add both as `tower` and `screen` landmarks. The pit building has VIP lounges above the garages; the kit row is a single storey (kit gap). |
| Landmark | - | The venue has no unique structure beyond the race tower (under S/F) and two advertising footbridges, which are laid. Possible horizon landmark: the Börde wind turbines. The dossier has 2 `power=generator` entries unclassified; if they are turbines, that is a kit gap. |
| Stands | rule | The real venue is a bowl of earth spectator banks 6-8 m above the track (dossier "Modelling pitfalls"). The overview map names Zuschauerbereich Nord, Nord-Ost, Ost, West, Mitte and Kart & Supermoto, and OSM has 15 `man_made=embankment` ways unclassified. No pass lays spectator banks outside fan_dressing, and no MANUAL_STANDS family is a bank. The grandstands are right: main, Hasseröder (966 R), West (1931 L) and Nord (2108 L). |
| Trees | rule | Three broadleaved woods give only 26 belt trees, while OSM has 15 `natural=tree_row` ways unclassified. Trees are planted only inside woods. The map shows a mostly open grass bowl edged with tree rows, so the rows (and the shrub areas) are what is missing. |
| Barriers | rule | 948 `tecpro_2m` (1.9 km) against 1105 armco, 0 tyres and 0 concrete, even though 19 walls are mapped. A compact club circuit with tyre stacks at T1, the Hotelkurve and the hairpin, and concrete pit and grandstand walls, comes out Tecpro-heavy with no tyres. |
| Boards | dossier | Only 3 corner signs (2 skipped in the pit zone). The Hasseröder-Kurve is not in `corners`; add it. The map also names the Profi-Car Schikane (2015) and the Triple. 436 hoardings and the braking boards are ok. Map POIs not dressed: TV Compound (11), helipad (15), Tankstelle (7), Arena Diner (16), Fanshop (17). |
| People | kit | Spectators on the banks all round (the bowl's whole character), the fan zone in the infield, and marshals. |

## Positions from the maps (mao-uebersichtskarte-2021.jpg)

Driving direction on the straight is westward, so the bottom of the map (pits) is left and the infield is right.
- **Race-Tower / Presse Zentrum (1).** The east end of the pit building, just before the Start/Ziel line, on the left. Station about 3620-3680 (Verwaltung (2) is at 3652 L, offset 26 m, and the tower stands beside it toward the line). Offset about 30-40 m. It is likely to fall in the pit zone, so place it at the row's end.
- **Zuschauerbereich Mitte (infield bank and fan zone).** On the right of the straight, behind the Zuschauertribüne, from about 3450 to 200 (the main stand's span) and back toward the Hasseröder. A large bank with the fan-zone screen. Put the screen behind the main stand near 0-50 R, about 60 m off.
- **Zuschauerbereich Nord.** Outside (left) of the north section, from about 2200 (after Nord-Tribüne 2108) to the Brücke at 2685, a long bank along the top of the map.
- **Zuschauerbereich Nord-Ost / Ost.** Outside (left) of the north-east and east bends, from about 2700 to 3330 (to the Zeppelin-Kurve 3334). The motorhome area (18) is behind the Ost bank.
- **Zuschauerbereich West.** Outside (left) of the west section, from about 1850 to 2100, between West-Tribüne (1931) and Nord-Tribüne (2108), around the Schikane (2015).
- **Hasseröder Tribüne.** The map shows it on the east side of the Hasseröder hairpin, facing it. The dossier has 966 R, offset 55 m, which matches. The Hasseröder-Kurve corner itself, missing from `corners`, is at about 900-1000.
- **Bridges.** "Brücke" over the west part of the straight (layout VW-Brücke 219) and "Brücke" at the top of the north section (2685). Both are footbridges laid as `truss_bridge`, which is ok.
- **Tunnels.** Two tunnels under the straight's east end (infield access), at about 3500-3600. Nothing to dress above ground.
- **4*-Hotel.** At the Hotelkurve, outside (left), structure at 477 L. It is laid as 3 x `media_centre`. `hospitality_3f` would be a closer stand-in via MANUAL_STRUCTURE_ASSETS (Sky Lounge on the 3rd floor).

## Kit gaps

- `pit/garage_6m_upper` (the VIP-lounge storey over the garages). Shared with Monza and Nürburgring.
- `misc/wind_turbine`, only if the `power=generator` entries prove to be turbines. They are a landmark of the Börde horizon.
- People family.

## Rule gaps

- No spectator-bank layer outside fan_dressing: `man_made=embankment` and named spectator areas ("Zuschauerbereich") are not used. Needs a bank rule (`spectator_bank_10m` runs, ideally with a raised terrain berm) or a MANUAL_STANDS `family="bank"`.
- `natural=tree_row` is never planted (15 rows here).
- Tecpro default and mapped-wall-to-concrete threshold, as at Nürburgring: 0 tyres and 0 concrete here.
## S/F dossier pass

In the tables:

- `MANUAL_LANDMARKS`: tower at 3625.0 left.

Left, or for later columns:

- Misplaced: the OSM "stand" "Boxen / VIP-Loungen" (55 L, -34..144) is the pit building (map item 3) with its lounges, not seating, yet 3 x bay_10m_large are laid there on the pit side, past the end of the generated garage row (boxes end at 38). No table drops an OSM stand (MANUAL_STANDS promotes buildings only, and this is a stand): it needs a drop-by-name for OSM stands (tooling) or an OSM fix. "Verwaltung" (3652 L, also an OSM stand) is skipped over the garage row, which is right.
- The generated garage row (24 boxes, 3591-38) sits mostly before the line, while the map draws the pit building after it (about 5-183; OSM -34..144). That is the export's pit layout, not a dossier table.
- Not placed: the DTM fan-zone big screen. The overview map has no screen symbol; the dossier puts it in the fan zone behind the main stand (Zuschauerbereich Mitte), facing the fan zone rather than the track. Fanshop (17) and WC (6) are drawn behind the main stand's west end (about 150-200 R), so a screen there would be a guess.
- For other columns: the Zuschauerbereich banks (rule gap), the two-storey pit building (garage_6m_upper kit gap), the 4*-Hotel as hospitality_3f.

## Landmark dossier pass

Nothing went in.

- Nothing fits the kit: the hotel (112 m) would shrink to a 30 m block, the DEKRA fan tower has no position.

## Stands dossier pass

In the tables:

- `STAND_FAMILY` `-`: "Boxen / VIP-Loungen" and "Verwaltung", the pit and admission buildings OSM tags as stands (R18), are left out; the Zuschauerbereich West, Nord, Nord-Ost and Ost banks from the overview map.

Left, or for later columns:

- Zuschauerbereich Mitte is hard standing: not laid.
- The raised berms and OSM's 15 embankment ways.

## Trees dossier pass

- The 15 tree rows (R8: no cached extract).
