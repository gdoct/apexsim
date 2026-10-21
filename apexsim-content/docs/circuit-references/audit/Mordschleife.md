# Mordschleife (Nordschleife) dressing audit

Lap 20764 m, starts and ends at T13 (20688). CircuitStyle NORDSCHLEIFE: vangrail, no hoardings, German signs, fan_dressing, no floodlights. The pit lane is "kept": a 190 m lane on the right mapped at T13 (the T13 pitstop area).
Maps read: 24h-lageplan-nordschleife-2020.jpg (campsite and spectator zones, roads, villages), oversteer48-corner-names.jpg (a word cloud, no positions). fastcar-nordschleife-cornering-map.jpg was not read (a corner-speed map).

| Column | Cell | Reason |
|---|---|---|
| S/F | dossier | The lap's start area at T13 has no grandstand: `stands` is empty and explain reports 0 stands. The dossier "Buildings" and "Start/finish" sections name the T13 grandstand and the T13 pitstop area (fuel pumps, toilet block). The lane is kept, but nothing marks it as a pit stop area (no fuel canopy, see kit gaps). The Döttinger Höhe gantry (17973, `timing_gantry`) and the toll lanes are there. |
| Landmark | ok | Burg Nürburg (castle_ruin, 19785 R), Kaiser-Wilhelm-Turm (13533 L), three toll lanes (19219 R), Antoniusbuche bridge (overpass at 19702) and the hillside letters (invented, on purpose) are all laid. Only the Steilstrecke is missing: the old concrete test hill beside the Karussell climb (corner "Steilstrecke" 11586). That would be a kit item, but it is low priority and mostly overgrown. |
| Stands | dossier | The only real permanent stand on the lap is T13, and it is missing. The 24h scaffold stands appear as `camp_scaffold_tower` in the camps, which is ok. Add T13 as a MANUAL_STANDS entry (see positions). |
| Trees | rule | 33 woods, all `mixed`. The Eifel forest is spruce-heavy with beech, so mixed is acceptable, but MIXED plants a fifth poplars: 1148 poplars laid, as many as broadleaf_l. Poplar has no place in the Green Hell. Forest impostors split 3173 broadleaf to 1910 conifer; spruce stands (Hatzenbach, Kesselchen/Klostertal, Hohe Acht) could be given `needleleaved` in the dossier, but the main fix is the species rule. |
| Barriers | rule | Vangrail is laid in single, triple and fence variants, which matches. But 1073 `tires_4m` (4.3 km) are laid while the dossier maps no tyre barriers. They come from geometry: `HEAD_ON_RADIUS_M` with very little run-off, because the style's corner minimum is 4.5 m. On the real Nordschleife, tyre stacks in front of the rail are spot fixes. Check the share against footage before changing the rule. |
| Boards | dossier | Chevrons, German road signs, km markers, rail tags, graffiti, 55 numbered marshal posts and 42 corner signs are all good. But the 24h campsites on the Lageplan are mostly missing. Laid: Brünnchen, Pflanzgarten, Schwalbenschwanz, Caravan Park and one unnamed. Missing: Hatzenbach, Adenauer Forst/Kallenhard, Metzgesfeld, Wehrseifen, Breidscheid/Ex-Mühle, Karussell/Steilstrecke, Hohe Acht, Wippermann/Eschbach. There is no MANUAL_AREAS table today, so this is a dossier layer that needs a table. |
| People | kit | This is the venue's character: 24h crowds on the banks at Brünnchen, Pflanzgarten, Karussell, Hatzenbach and Wehrseifen, the campers, and marshals at the numbered posts. Wanted: standing crowd on the `spectator_bank_10m` runs, campers by the gazebos, and marshals with flags. |

## Positions from the maps

From 24h-lageplan-nordschleife-2020.jpg. The green areas are "Zuschauerzonen mit Campingmöglichkeit". Stations come from the layout corners; side is relative to driving direction. The lap runs clockwise, so the loop's inside is on the right.
- **Hatzenbach camp/spectator zone.** Hatzenbach 1039 to Hocheichen 1482, outside (left) of the run, a long narrow belt west of the track.
- **Adenauer Forst to Kallenhard zone.** About 5100-6300 (Adenauer Forst 5169, Metzgesfeld 5903, Kallenhard 6329), outside (left), a belt along the access road north of the track.
- **Wehrseifen/Breidscheid zone.** About 7300-7800 (Wehrseifen 7389, Breidscheid 7713), on the Adenau side (left), a belt down to the B257.
- **Karussell to Hohe Acht zone.** About 11900-12800 (Karussell 11998, Steilstrecke 11586, Hohe Acht 12748), between the Karussell climb and the Steilstrecke, on the right (inside).
- **Brünnchen/Eschbach/Wippermann.** About 13400-14400. Brünnchen is laid; Wippermann/Eschbach (13474-13905), right toward the B412, is missing.
- **Pflanzgarten/Schwalbenschwanz.** Laid.
- **T13 grandstand.** Not on the 2020 Lageplan (it shows stands only on the GP: the dotted blue lines). Dossier: "between Hohenrain-Schikane and Sabine-Schmitz-Kurve, at the junction with the GP circuit". Stations about 20500-20764/0-165 (Hohenrain 20471, T13 20688, Sabine-Schmitz 165). Probably on the right, beside the T13 pitstop lane (the lane is mapped right). Fix the side and the extent on the trace page before writing MANUAL_STANDS.
- **Media tower (webcam).** Dossier: overlooking the start straights at the GP/T13 junction, position unconfirmed. If it is confirmed, a `camera_tower` or `tower` landmark.

## Kit gaps

- `building/fuel_station_canopy`: the T13 pitstop area pumps, the ED/"Döttinger Höhe" station (fuel POI at 17961) and Aral at Breidscheid (8059). None of these POIs is dressed today.
- `misc/steep_test_ramp` (Steilstrecke): low priority.
- People family (see above).

## Rule gaps

- `poplar` sits in the NEEDLELEAF and MIXED species sets in groom.rs: 1148 poplars in the Eifel forest.
- Geometry-driven tyre walls on a narrow-verge style (4.5 m corner minimum): 1073 modules with none mapped. A Vangrail-style circuit may want tyres only where mapped.
- Spectator banks exist only as fan_dressing beside mapped car parks and camps. Zones the Lageplan shows without a mapped area get none.
## S/F dossier pass

Nothing goes in the tables yet:

- Station 0 is at T13, not on the Doettinger Hoehe: the layout's T13 corner runs 20638-74 with station 0 inside it, the Hohenrain-Schikane is at 20471 and the Sabine-Schmitz-Kurve at 165. The Doettinger Hoehe gantry (17973) and the toll lanes (19219 R) are 1.5-2.8 km before the line and are not S/F. The kept T13 pit lane runs 20638-75 on the right, 8 m from the centerline. Nothing else stands within 150 m of the line apart from the GP circuit's workshops (20640-20670 L, 38-120 m off).
- The GP circuit beside T13, from the five OSM buildings both dossiers share (the two "Werkstaetten" and three unnamed blocks; a rigid fit with zero residual): the GP's Coca-Cola corner and the start of its pit straight (GP 4550-4630) run 60-130 m to the LEFT of the Nordschleife from 20600 to 160, and the NGK-Schikane (GP 4350-4440) 70-90 m to the left at 280-400. So anything between the two roads at T13 is on this lap's left, not beside the T13 pit lane on the right as the audit guessed.
- T13 grandstand: not placed. No map in maps/Mordschleife places it (the 2020 Lageplan draws stands only on the GP, schematically), and the dossier says only "at the junction with the GP circuit". If it overlooks the GP's final chicane (the Nuerburgerring audit's 4400-4600 L), it stands between the two roads, on this lap's left, with its back to this lap. Needs the trace page or an aerial before an entry.
- Media tower (webcam): position unconfirmed in the dossier; not placed.
- The T13 pitstop area's two fuel pumps and toilet block need the `building/fuel_station_canopy` kit gap; nothing in today's kit marks the lane as a pit stop area.

## Landmark dossier pass

In the tables:

- `MANUAL_LANDMARKS`: three `fuel_station`s on OSM's filling stations in sight of the lap: ED Döttinger Höhe (node 1711333738), Aral at Breidscheid (way 1023567856, standing for OSM's canopy roof) and TotalEnergies in Nürburg (way 114676264).

Left, or for later columns:

- The T13 pumps: no position, and in the pit zone.
- The Steilstrecke: a smaller kit ask.

## Stands dossier pass

In the tables:

- The T13 grandstand from OSM way 34435441 (tagged bleachers only, R22), covered, behind the T13 lane; eight banks for the 24h map's spectator zones without a camp (Hatzenbach, Adenauer Forst, Wehrseifen, the Karussell wedge, Hohe Acht-Wippermann).

Left, or for later columns:

- The zones are scaled from a schematic map (+-100 m).
- The media tower: no position.
