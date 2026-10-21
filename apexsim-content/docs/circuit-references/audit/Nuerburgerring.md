# Nuerburgerring (Nürburgring GP-Strecke) dressing audit

Lap 5141 m, clockwise, pit lane right (inside), 952 m long. The pit building is an OSM `garage` 303 m long, centred on station 5127 R, so the straight runs about 4790 to 5141 and 0 to 400 (DRS zone 2 is 4792-367). Only six corners are in the layout: Sprintstrecke 818, Anbindung Müllenbach 1399, Ford-Kurve 1627, Schumacher-S 2694, Anbindung Sprint 2975 and NGK-Schikane 4350. T1 Yokohama-S, Mercedes-Arena, Valvoline, Dunlop-Kehre, Kumho, Bit, Advan and Coca-Cola are missing.
Maps read: racingcircuits-gp-f1chicane-arena-03.png and racingcircuits-muellenbach-03.png (corner names and layout only; they place no structures). fia-2020-eifel-circuit-map.pdf was read later from its rendered PNG (see the section at the end). No Nuerburgerring entries exist in MANUAL_STANDS, MANUAL_LANDMARKS or MANUAL_CROSSINGS.

| Column | Cell | Reason |
|---|---|---|
| S/F | dossier | The main grandstand T3 (BMW M Power, opposite the pits, permanent video wall) is not in `stands`, so nothing faces the pits. The BMW M bridge over the straight (BMW-Brücke, 4958, OSM kind `road`) falls through to the default and is laid as a `tyre_bridge`, the Le Mans Dunlop look. The real one is a footbridge or link bridge: set CROSSING_KINDS `road` to `link`, as Mordschleife maps `road` to `overpass`. The TÜV Rheinland Tower (race control, 4889 R) is skipped as part of the pit complex. The ring°boulevard LED wall (45 x 8 m) is missing. The pit building is multi-storey with lounges, but the kit row is a single storey (kit gap). |
| Landmark | dossier | Burg Nürburg overlooks the circuit, but it is in the Mordschleife table only. NUERBURG_CASTLE already exists in osm_layout.py, so add it to MANUAL_LANDMARKS for Nuerburgerring. The unnamed `tower` landmark at 1457 R is laid as `control_tower`; check what it is (DEKRA fan tower or a mast). |
| Stands | dossier | Missing: T3 (main), T12/T12a (Advan-Bogen) and T13 (Nordschleife junction). Laid: Tribüne 4 (333 L, outside T1), Mercedes T4a (1142 L), T5a, T5b, T6, T8, T9, T10a and T10b. Also a rule issue: T5b has depth 186.5 m and T8 109.6 m, because the OSM polygons are whole spectator hillsides. The 25 m `ASSUMED_ROOF_DEPTH_M` rule lays them as `bay_10m_large_roof`. |
| Trees | dossier | Nineteen woods are `mixed`, two `needleleaved` and one `broadleaved`. Forest impostors split 231 broadleaf to 19 conifer, while the Eifel plateau's woods are mostly spruce. Set leaf types per wood. Only about 100 belt trees are laid, which is ok for an open plateau. |
| Barriers | rule | The explain output lays 1709 `tecpro_2m` (3.4 km) and 1596 armco, but only 17 tyre modules and 0 concrete, even though 83 walls are mapped. The real GP-Strecke has concrete walls and debris fence on the straight, armco behind gravel, and tyre walls at T1 and Dunlop. Tecpro is over-used and mapped walls never become concrete. |
| Boards | dossier | Only 3 corner signs, because the layout has only six corners and three of them have no display name. Add the named corners (T1 Yokohama-S to T15 Coca-Cola) with CORNER_DISPLAY parodies. 500 hoardings, 15 braking boards and many lettered car parks are fine. The rendered FIA 2020 map (p2) shows about 51 marshal posts (M1-M50 plus M40a) and 18 FIA light panels. The explain output lays 13 `marshal_post` and 20 `light_panel`, so posts are about a quarter of the real number. The surroundings rule puts a post only at each named corner and every 400 m, so this is also a rule gap. |
| People | kit | Crowds in T3 and the Müllenbach stands come from crowd cards. Spectator hillsides (T5b and T8 are really banks) and marshals have no figures. |

## Positions from the maps / dossier text

The maps read place no structures; positions below come from dossier text against layout stations. Check each on the trace page.
- **T3 main grandstand (BMW M Power).** Left side, opposite the pit building. Pits are at 4975-5141 and 0-140 on the right, so T3 is at about 4980-5141 and 0-100 on the left, in front of the ring°boulevard (structure "Ring Boulevard" 5123 L, offset 45 m). Covered, 5000 seats, depth about 25 m. Add it to MANUAL_STANDS with covered=True and a `screen` landmark on its roof or end (the permanent video wall).
- **BMW M bridge.** OSM crossing at 4958, which is right. Only the kind needs fixing (`link`).
- **Unnamed crossing at 1236.** Also kind `road` and laid as `tyre_bridge`, between the Sprintstrecke junction (818) and the Müllenbach link (1399). Check on the trace page what it carries (probably a service road or footbridge) and give it a kind.
- **T12 / T12a.** At the Advan-Bogen, the right kink on the back straight. DRS zone 1 runs 3938-4293, so about 3950-4200. Probably on the outside (left); unconfirmed.
- **T13 stand.** Beyond the NGK-Schikane (4350), outside the circuit at the Nordschleife junction, about 4400-4600 on the left. Unconfirmed; it is shared with the Mordschleife dossier's T13.
- **TÜV Rheinland Tower.** 4889 R, offset 20 m, inside the pit-complex skip. It is the race-control tower, so either a `tower` landmark placed just clear of the pit zone, or accept that the pit complex stands in for it.
- **ring°boulevard LED wall (ring°wall).** On the boulevard facade, left of the straight. A `led_wall_stage_16m` or `video_screen` landmark there.

## From the rendered FIA 2020 map (fia-2020-eifel-circuit-map.pdf p.2; p1 is the cover sheet)

It is a race-control map: corners, marshal posts and light panels, with no buildings or stands. What it adds:
- **Pit lane.** Drawn on the inside (right) of the straight, from T15 to past T1, which confirms side `right`.
- **Control line and start line.** The control line (chequer) is near the T15 end of the straight. The start line is about 263 m further on, toward T1. So the chequer and the gantry are not at the same station: the finish line is the control line, and the start lights sit at the start line.
- **Marshal posts and light panels.** Posts M1-M50 plus M40a, spread at roughly 100 m pitch all round, densest through T1-T4 (M1-M11) and T11-T15 (M33-M50). Light panels 1-18: 1 near the control line, 2 on the straight's T1 end, 3-7 through the arena, 8-11 in the Schumacher-S and Dunlop section, 12-15 on the back section, 16 before T13, 17-18 at T14-T15. These are positions only; the counts are a dossier `MANUAL_FURNITURE` or rule question.
- **Corner numbering.** T1-T15 in FIA order, which gives the missing named corners their order. T7 is the Dunlop hairpin in the far south-west, T11 the Bit-Kurve, T13-14 the chicane and T15 Coca-Cola.
- No structure positions. T3, T12 and T13 stay as given above (dossier text plus the trace page).

## Kit gaps

- `pit/garage_6m_upper` (an upper lounge storey) for the multi-storey 33-garage building. Shared with Monza and Oschersleben.
- People family.

## Rule gaps

- A crossing of OSM kind `road` with no CROSSING_KINDS entry defaults to `tyre_bridge`. That default suits only Le Mans.
- `ASSUMED_ROOF_DEPTH_M` reads OSM bounding-box depth. Hillside spectator polygons (T5b 186 m, T8 110 m deep) become huge roofed large stands. Cap the depth or treat them as spectator banks.
- `barriers::decide` lays Tecpro on the outside of every bend without ample run-off, and lays concrete for mapped walls only at near-zero run-off. Here that gives Tecpro over armco and 0 concrete from 83 mapped walls.
## S/F dossier pass

In the tables:

- `MANUAL_STANDS`: T3 (5000-120 left covered).

Left, or for later columns:

- The extent is the doubtful part: the dossier says only "opposite the pits", so the 260 m is an estimate from the pit building, good for the stand's side and middle, not its ends.
- The BMW-Bruecke (4958, OSM kind `road`) is laid as `tyre_bridge`, the Le Mans look, over the straight. It is a footbridge/link bridge: CROSSING_KINDS "Nuerburgerring": {"road": "link"} (as Mordschleife maps road to overpass). That also re-kinds the unnamed crossing at 1236; check that one on the trace page.
- The FIA map's control line sits about 200 m after the T15 exit (about 4850) and the start line 263 m further on (about 5110), so the YAML's station 0 is near the start line, not the control line.
- The generated pit boxes run 4857-4995 R, at the T15 end of the 303 m OSM garage (4975-137); the TUeV Rheinland Tower (4889 R, race control) is skipped inside the pit complex. Left to the pit complex.
- T13 (Nordschleife junction): the Nordschleife runs 60-130 m to the left of GP 4360-4630 (shared OSM buildings, see the Mordschleife draft). A stand there would be on the left between the two roads, but no map places it; not added. T12/T12a and the multi-storey pit building (garage_6m_upper kit gap) are for other columns.

## Landmark dossier pass

In the tables:

- `MANUAL_LANDMARKS`: Burg Nürburg (`castle`) on the skyline, the Nordschleife entry's anchor.
- `MANUAL_CROSSINGS`: the BMW bridge as a covered `link` (OSM bridge=covered); the lap's own bridge at the Müllenbach link as `deck_track` from OSM way 1443047846's end nodes, where the scan's `road` crossing (the side bridges) would have stood a concrete bridge across the track.
- `LANDMARK_KINDS`: OSM's untyped tower at 1457.3 right is a `camera_tower`, not race control.

Left, or for later columns:

- The TÜV tower, the boulevard LED wall and the DEKRA tower: other columns.

## Stands dossier pass

In the tables:

- T3 moved onto OSM's "BMW Tribüne" outline (R22); T7 and T12a (right) added; T5b and T8, whole spectator hillsides, as banks.

Left, or for later columns:

- T12 itself (OSM has only T12a); the roofs of Tribüne 4 and Mercedes T4a; T13 faces the Nordschleife.
