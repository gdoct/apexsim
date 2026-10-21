# MandarinaBay (Marina Bay) dressing audit

Sources read: dossier `circuit-references/MandarinaBay.md`; maps
`sgp-circuit-park-map-2026.pdf p.1` (official 2026 Circuit Park Map,
labelled "2027"/"15 Oct 2026": every stand, superscreens, podium, zones),
`oversteer48-grandstand-map-2025.jpg` (clean schematic, turn numbers and stand sides).
FIA pit-lane PDF renders and the F1 webp not needed for the stands. Explain
the `ats-dress --explain` output; `MandarinaBay.layout.json`; MANUAL_STANDS / MANUAL_LANDMARKS /
CROSSING_KINDS / MANUAL_STRUCTURE_ASSETS `["MandarinaBay"]`; style STREET.

Turn stations (no named corners in the YAML or layout, only one "Marina Bay Street Circuit"
corner; read from centerline heading changes and matched to the maps' turn order; good to
~20 m): T1 ~420 (L), T2 ~490 (R), T3 ~570 (L), T4 ~700 (kink), T5 ~970 (R), T6 ~1300 (kink),
T7 ~1800 (L), T8 ~2010 (R), T9 ~2210 (L), T10 ~2660 (L), T11 ~2790 (R), T12 ~2850 (L),
Anderson Bridge 2912-2983, T13 ~3050 (L hairpin), Esplanade Bridge 3067-3359, T14 ~3590 (R),
T15 ~3750 (kink), T16 ~4340 (R), T17 ~4410 (L), ECP viaduct 4447, T18 ~4700 (L), T19 ~4800 (L);
lap 4921. Anticlockwise; pit lane left, 4582 -> 519.

## Cells

| Column | Cell | Reason |
|---|---|---|
| S/F | rule | Pit building (`pit_building_roofdeck` for the street style), Pit and Super Pit stands (roofed `street_stand_tier_10m_roof`, right) and the Flyer behind all match. Lost: **Pit Exit Grandstand** (inside before T1) and **Chicane @ Turn 2** (inside T2-T3), explain "skipped stand ...: nothing to build clear of the pit lane": both stand *behind* the pit-exit lane (park map), but the pit zone keeps the garage depth along the whole lane, not only where garages stand. Also dossier: "Republic Grandstand" is laid on the pit straight (215-330 right), where no map has it. No superscreen on the straight (park map has them; `video_screen` exists, needs `screen` landmarks). |
| Landmark | ok | Flyer (`landmark_big_wheel_xl`, inside the final loop, as the park map), three-tower hotel, lotus museum, double helix, Esplanade domes, Fullerton, Old Supreme Court, City Hall, St Andrew's, Suntec, ECP viaduct (`viaduct_deck`), Anderson (`bridge_arch_steel`) and Esplanade (`bridge_deck_wide`) bridges are all laid. |
| Stands | dossier | MANUAL_STANDS (from the 2025 map, "good to +-100 m") misplace three: Republic (on the pit straight; real at T4-T5, drivers' right), Raffles (3980-4100 on Raffles *Avenue*; real at T5 exit on Raffles *Boulevard*, drivers' left), Bayfront (4545-4640 left; real at T17 under the ECP, outside/right, which also takes it off the pit lane). Skyline is right and Promenade left in the table, where the dossier ("Skyline: drivers' left between T17 and T18") and oversteer48 put Skyline left / Promenade right: check on the trace page. Superscreens at the Padang, Connaught, Bayfront and T1 (park map) are not laid. |
| Trees | ok | 323 `raintree_l` from OSM tree nodes (city POI layer) along the Esplanade, Padang edges, Raffles Ave; the Padang itself is open lawn as in reality. |
| Barriers | rule | 4791 `tecpro_2m` + 12 corners, nothing else: the STREET style lays Tecpro everywhere (`the_street_style_is_tecpro_and_paving`). A street circuit is concrete blocks with a debris fence on the straights and Tecpro only at the run-off ends of corners; the dossier pitfalls call out "temporary ... walls, fences ... and the floodlight gantries". Kit has `concrete_4m_rail`, `catch_fence_6m`, `catch_fence_post_lit_6m`. |
| Boards | rule | 2408 hoardings, 644 LED ribbons, banner gantries are right. But only 8 marshal posts and 0 corner signs: no named corners in the dossier, and `marshal_posts` falls back to 400 m spacing (4 lost inside buildings/water); there is no manual corner table. The night race's light-truss pylons are not laid (only street lamps `lamp_arm_twin`, `lamp_globe_pole`). |
| People | kit | Every stand and the Padang/Esplanade viewing platforms (park map "viewing platform" icons), F1 Village zones, marshals. Wanted: seated crowd on the street stands, standing crowd on viewing platforms and in the Padang and Zone 1 village, marshals. |

## Positions from the maps

| Structure | Where | Side | Extent | Map |
|---|---|---|---|---|
| Republic Grandstand | T4 -> T5 run, before T5 | right (outside of T5) | ~850-960 (move from 215-330) | park-map-2026 (REPUBLIC by T5), oversteer48 (Republic beside the T4-T5 leg) |
| Raffles Grandstand | T5 exit, along Raffles Boulevard | left | ~990-1090 (move from 3980-4100) | park-map-2026 (RAFFLES below T5), oversteer48 |
| Bayfront Grandstand | T17, under the ECP viaduct | right (outside) | ~4380-4440 (move from 4545-4640 left) | park-map-2026 (BAYFRONT at T17), oversteer48 (west of 17) |
| Skyline / Promenade | T17-T18 run | Skyline left, Promenade right (layout has the reverse) | ~4470-4680 | oversteer48, dossier; park map ambiguous |
| Marina Bay Grandstand | T18 | right (outside) | as MANUAL (4685-4725) | both |
| Pit Exit Grandstand | inside before T1, behind the pit-exit lane | left | 300-395, needs ~25 m more gap (beyond the lane) | park-map-2026 (PIT EXIT GRANDSTAND west of the exit line) |
| Chicane @ Turn 2 | inside T2-T3, behind the pit exit | left | 470-540 | park-map-2026, oversteer48 |
| Turn 1 / Turn 2 Grandstands | outside T1-T2 | right | as MANUAL | both |
| Podium | pit side, by the line on the pit building | left | ~4880-0 | park-map-2026 (podium icon by the chequer) |
| Superscreens | facing the pit straight (inside, ~4800), T1 (~400 right), Padang (~2400), Connaught (~3500), Bayfront/T17 (~4420), Raffles Ave (~4000) | various | - | park-map-2026 (superscreen keys) |
| Wharf / Padang / Esplanade stages | Zone 1 village north of T1-T3; the Padang (inside T9-T10); Esplanade outdoor theatre | inside | - | park-map-2026 (stage icons) |

## Kit gaps

- `light/light_truss_pylon` (night-race lighting gantry along the track) if
  `catch_fence_post_lit_6m` is not enough on its own.
- Figures (People).

## Rule gaps

- PitZone: stands behind the pit-exit (or pit-entry) lane are dropped ("nothing to build clear
  of the pit lane"); the garage depth should apply only along the garage row, and a stand
  beside the lane should step back past it. Loses 2-3 stands here (and T34 at Le Mans).
- STREET barrier style: concrete + debris fence (lit posts) on straights, Tecpro only at corner
  run-off ends, instead of Tecpro on every cell.
- No manual corner table (as at Hungoverring): 0 corner signs, 8 marshal posts.
