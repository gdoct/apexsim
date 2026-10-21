# Monzarella (Monza) dressing audit

Lap 5780 m, clockwise, pit lane right (east). Corners (layout.json): Rettifilo di partenza 492, Variante del Rettifilo 954, Curva Biassono (Curva Grande) 1509, Roggia 2151, Lesmo 1 2550, Lesmo 2 2876, Serraglio 3299, "anello alta velocità" 3594, Vialone 3945, Ascari 4087, Alboreto/Parabolica 5280.
Maps read: 2024-monza-seating-map.jpg (stand numbers by place), 2026-italian-gp-spectator-map.png (old oval, pedestrian bridges, tunnels), f1-2026-track-detailed.webp (corners only). The two PDFs (2026 wayfinding, 2024 circuit map) were read later from their rendered PNGs (see the section after the positions).
There are no Monzarella entries in any MANUAL_* table; everything comes from OSM.

| Column | Cell | Reason |
|---|---|---|
| S/F | dossier | Generated pit complex (garage_6m row and a podium on the middle roof) stands in for the Palazzina Box. The explain output skips "Palazzina Box" and "Hospitality Building" as part of the pit complex, which is right. The opposite stands are there (Centrale 315 L, Laterale Sinistra 401 L, Laterale Destra 126 L, Vedano 5638 L, Piscina 500 L), but all are `covered: False` from OSM and laid unroofed (`bay_10m_large`). The dossier's "Buildings" section has 1, 4, 24 and 26 covered. There are no screens at all (no `screen` landmark). Also: the real pit building has two storeys with the Paddock Club terrace on the roof (2024 roof), but the kit's garage row is a single storey (see kit gaps). |
| Landmark | kit | The old banked oval (Sopraelevata) is the venue's landmark. The rendered 2026 wayfinding and 2024 circuit maps show it crossing over the GP road only once: the north banking over the Serraglio-Ascari approach (about station 3550-3600). It leaves the pit straight's line at the Rettifilo, runs outside the back straight behind stands 17/20/21, and its south banking curves behind the Parabolica's outside stands. That corrects the dossier's "before the Roggia" bridge, and the earlier reading of a crossing at 1100-1250. Today the only thing there is OSM `footbridge` 3586, laid as `truss_bridge`. No kit piece looks like a concrete banking on piers. Torre Nord (584 L) and Torre Sud (5777 L) are laid as `control_tower`; the maps confirm both on the left, one either side of the line. |
| Stands | dossier | Placement matches the seating map well. The losses: stand 23 (Tribuna Parabolica interna, 5570/5620 R) is skipped twice with "nothing to build clear of the pit lane", and the covered flags are missing on 1/4/22/24/26. Tribuna Parabolica (22, permanent and covered) is laid as `bay_10m_large` without a roof. Prima Variante Esterna (8) gets a roof only through the 25 m depth rule; check whether the real one is roofed. There is no table today for patching an OSM stand's `covered` (MANUAL_STANDS only adds stands), so this needs a small tooling addition or MANUAL_STANDS replacements. The rendered PDFs add stands that are in the dossier nowhere: 17 and 20 (outside the back straight), 11 (Serraglio), 33, 34, 10bis, 32, 35 and 31. Positions are below. |
| Trees | dossier | Nine woods, eight of them `mixed`. The Parco di Monza is deciduous parkland (oak, hornbeam, plane, chestnut), and the "Park woodland" row says Lesmo and Serraglio are in trees. Mixed plants conifers and poplars: groom gives 180 conifer_m(_near) and 149 poplar. Set the park woods to `broadleaved`. The rule gap on poplar applies too (see below). |
| Barriers | rule | Seven walls are mapped, yet no `concrete_4m` is laid anywhere (explain: armco 1982, armco_fence 500, tecpro 739, tyres 77). The real pit straight's stand side is a concrete wall with a tall debris fence. No pass lays `catch_fence_6m`. |
| Boards | dossier | Only the fan zone is missing. The 2026 wayfinding map puts the F1 Fanzone (stage and flagship store) outside the back straight, inside the oval: about 4450-4600 L, 100-150 m off. Add it as a `stage` landmark. The rest is fine: 654 hoardings, 150/100/50-style braking boards (6 sets), 22 light panels, 14 marshal posts and 9 corner signs. Gates and food stalls come from POIs. This matches an F1 venue. |
| People | kit | Tifosi on the pit straight and under the podium, crowds on the Ascari and Parabolica GA/gradinate (GF10-13 inside the Parabolica), and marshals at 14 posts. Wanted: seated crowd cards exist for stands, so standing crowd figures on GA banks, a track-invasion crowd for the podium, and marshals in orange. |

## Positions from the maps

- **Old oval, crossing 1 (after the first chicane).** The 2026 spectator map shows the grey "Old Circuit" loop's north end crossing the GP road just after the 1st Variante, where 6C sits "behind part of the old banking" (dossier T1 notes). Estimate: station about 1100-1250, between Variante del Rettifilo 954 and Curva Biassono 1509. The banking spans the road on piers. It is not in `crossings`: add a MANUAL_CROSSINGS entry anchored with the trace page. The kind is a banking span (kit gap), with `viaduct` as a stopgap.
- **Old oval, crossing 2 (Serraglio, approach to Ascari).** Station 3586, already an OSM `footbridge` laid as `truss_bridge`. Re-kind it to the banking span (CROSSING_KINDS or MANUAL_CROSSINGS). The banked curves themselves run beyond the road on both sides (spectator map, from the Ascari side round past the paddock to Mirabello/G): a landmark run, not just a bridge.
- **Stand 23 (Parabolica interna).** Seating map: inside T11, next to the pit entry. Dossier stations 5570-5640 R, offset 14-20 m. It is skipped because its front lies over the pit entry lane. Needs a MANUAL_STANDS entry with `gap_m` beyond the lane (about 30 m from the edge), or a check on the trace page that it really stands behind the lane.
- **GF10-GF13 (inside the Parabolica).** Seating map: a curved row of small stands on the inside of the Parabolica, from its apex to its exit. Dossier "Tribune" entries 5415-5524 R, with 9 bays laid. Ok.
- **Pedestrian bridges.** Spectator map: pink pedestrian bridges over the back straight near stands 17/18 and 21. Dossier crossings 4427 and 4721, so ok. The spectator map also shows bridges over the pit straight near the paddock entry. Positions are not readable at this resolution, so check against the PDF wayfinding map.
- **Screens.** No position on the readable maps. Monza has big screens opposite the pits and at the Parabolica/Ascari. They need the wayfinding PDF; add them as `screen` landmarks.

## Positions from the rendered PDFs (2026-italian-gp-wayfinding.pdf p.1, 2024-italian-gp-circuit-map.p1.png)

North points left on both maps. The pit straight runs right to left, and the paddock and pits (above the straight) are on the right. Clockwise, so the circuit's inside is on the right.

**Correction:** the "Old oval, crossing 1 (after the first chicane)" entry above is withdrawn. Both maps show the oval meeting the pit straight's line at the Rettifilo without crossing the GP road there. There is one road-over-road crossing, at the Serraglio-Ascari approach.

- **North banking crossing.** About station 3550-3600, at the "anello alta velocità" OSM corner (3594), between Serraglio (3299) and Vialone (3945). Both maps also show a pedestrian bridge just after it, toward stand 12. So OSM footbridge 3586 may be that footbridge rather than the banking. Check on the trace page, then add the banking as its own MANUAL_CROSSINGS entry.
- **Banking beside the road (landmark run).** Outside (left) of the back straight, about 60-120 m off, from the Serraglio crossing to past the Parabolica (behind stands 17, 20, 21A-E and 22). Then it curves round behind the Parabolica's outside.
- **Pedestrian bridge over the pit straight.** Between stand 7 and stand 6A, before the Rettifilo braking zone, about 750-850 (both maps, pink arrow). It is not in `crossings`.
- Pedestrian bridges over the back straight near 18/5 and near 21: these are the OSM 4427 and 4721 entries, so ok.
- **Stand 17.** Outside (left) of the back straight, right after the Ascari exit, about 4250-4350 L. Missing.
- **Stand 20.** Outside (left) of the back straight, mid-way, opposite 18-19, about 4450-4550 L. Missing. Stand 19 sits with 18 on the inside (2024 map "18-19-20" label; the 2026 map has 20 separate on the outside).
- **Stand 11.** Inside (right) of the Serraglio run, about 3150-3300 R. Missing.
- **Stand 33 (2026).** Inside (right) between Curva Biassono and the 2ª Variante, about 1900-2050 R. Missing.
- **Stand 34 (2026).** Outside (left) of the 1ª Curva di Lesmo, about 2480-2560 L. Missing.
- **Stand 10bis.** Inside (right) between Lesmo 1 and Lesmo 2, about 2700-2850 R. Missing.
- **Stand 32 (2026).** Inside (right), next to 6A by the pit exit, about 600-700 R. It may be the OSM "tribune" at 408 R that is skipped for the pit lane. If so, it needs `gap_m` behind the lane.
- **Stand 35 (2026).** Pit straight, left, past 24 toward the Parabolica exit, about 5600-5680 L. Missing.
- **Stand 31.** Disabled-access stand on the outside of the 1ª Variante next to 8, about 1000-1050 L. Possibly part of the Prima Variante Esterna polygon.
- **Stands 13-16, 12, 18, 6A-C, 8/8bis, 21A-E, 22, 23A-B, GF.** They match the dossier stands.
- **Screens.** Neither map has a screen symbol, so the maps give no screen positions.
- **Fan zone (stage and flagship store).** Inside the oval, outside the back straight, about 4450-4600 L, 100-150 m off.
- **Hospitality Building (B), Monza Village (M), Garden Lounge (A), Race Club (C), Fans Club (D).** In the paddock behind the pits, around 100-400 R. They are covered by the pit complex and OSM buildings.

## Kit gaps

- `bridge/banked_oval_span`: the concrete banked oval on piers crossing the road (Sopraelevata). Possibly also `building/banked_oval_curve` sections for the banking seen beside the road. Used here only.
- `pit/garage_6m_upper` (or `pit/pit_building_terrace_6m`): an upper storey with a glazed lounge or roof terrace stacked on `garage_6m`, for the Palazzina Box (and Nürburgring, Oschersleben).
- People family: standing spectators on GA, a podium crowd, marshals.

## Rule gaps

- `catch_fence_6m` / `catch_fence_post_lit_6m` are in the kit (props.rs) but no barrier kind or pass lays them.
- Mapped walls become concrete only with run-off ≤ `WALL_AGAINST_ROAD_M`. Monza has 7 mapped walls and gets 0 concrete.
- `groom.rs` NEEDLELEAF includes `poplar`, and MIXED is a fifth poplar. Poplars are broadleaf riparian trees, wrong for park or forest woods.
- No way to set `covered` on an OSM stand (osm_layout tooling).
## S/F dossier pass

In the tables:

- `MANUAL_STANDS`: Tribuna 25 (5715-5765 left).
- `STAND_COVER`: Tribuna Centrale covered; Tribuna Laterale Sinistra covered; Tribuna Laterale Destra covered; Tribuna Vedano covered.

Left, or for later columns:

- The order of the OSM pit-straight stands matches both maps: 26 (126 L), 1 (315 L), 4 (401 L), 5 Piscina (500 L), then the unnamed 50 m outline at 707 L, which is where both maps put 7 (Biassono). It can be named only in OSM or by a tooling change (it is a stand, not a building, so a MANUAL_STANDS promotion does not apply).
- Not placed: 27-30 (2026 map "30-29-28-27", drawn as one row in front of 26A-C; the 220 m OSM Laterale Destra outline most likely holds them), 2-3 (drawn in front of 4; the OSM gap between 4 and 5 is 21 m), 35 (2026 map, drawn in front of 24, not separable from Vedano within 50 m) and CC (a hospitality box, not a stand).
- The OSM "tribune" at 408 R (behind the pit lane by the exit, laid as 2 bays clear of the lane) may be the 2026 map's 32, which that map draws about 100 m later (470-545 R). Left as it is.
- The 2026 map's chequer is about 70 m after the YAML's station 0 (map scaled on the OSM stands); the OSM outlines were trusted throughout.
- Torre Sud (5777 L) and Torre Nord (584 L) sit 4 and 8 m off the left road edge; the 2026 map also draws both against the road. Left alone.
- For other columns: Tribuna Parabolica (22) is permanent and covered (dossier) but laid unroofed; the 2026 stands 11, 17, 20, 31-34 and 10bis (Stands); the podium and the two-storey Palazzina Box are the pit complex and the `garage_6m_upper` kit gap; the pedestrian bridge over the pit straight between 7 and 6A (about 750-850) is a MANUAL_CROSSINGS item.
