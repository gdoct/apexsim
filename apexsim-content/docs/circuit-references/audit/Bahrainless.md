# Bahrainless (Bahrain International Circuit, Sakhir) - dressing audit

Sources: dossier `circuit-references/Bahrainless.md`; maps
`gpgt-circuit-map-bahrain.webp` and `oversteer48-main-grandstand-seating.jpg`
(both read as images). `fia-2024-bahrain-circuit-map-red-zone.pdf` could not
be rendered (no PDF renderer on this machine). Also the `ats-dress --explain` output,
`Bahrainless.layout.json`, `osm_layout.py` MANUAL_LANDMARKS "Bahrainless"
(Sakhir Tower) and CORNER_DISPLAY "Bahrainless", and
`circuit_style::SAKHIR`.

Station frame: the dossier has one corner only (OSM "Grand Prix Circuit",
display None), so this is read off the curvature. S/F is at 0 and the lap
is 5392 m. T1 R 690-780, T2 L 800-860, T3 R 930-970, T4 R 1490-1600,
T5 L about 1800, T6 R 1850-1930, T7 L 1970-2050, T8 R hairpin 2210-2310,
T9-T10 L 2580-2760, T11 L 3400-3630, T12 R 3730-3910, T13 R 4020-4170,
T14 R 4870-4950, T15 R 4980-5010.

## Cells

| Column | Cell | Reason |
|---|---|---|
| S/F | kit | The Main Grandstand (OSM, 349 m, left, laid as 9 x `bay_10m_large_roof`), the pit lane (777 m, right) and the generated garages are all in place. The venue's signature is the **white tent-like PTFE fabric roofs** on the pit building and the main grandstand (dossier S/F "Pit building", pitfalls "Buildings use white tent-like fabric roofs"), and no kit piece has them. Dossier: the 3 TV screens on the pit building facing the grandstand (oversteer48 seating map) are not laid. Rule: the podium belongs at the entry end of the garages (seating map, dossier), but `ue_export` always puts it on the middle garage. |
| Landmark | kit | The Sakhir Tower is authored (MANUAL_LANDMARKS, `tower`, 582.3 R, 73.9 m off) and laid as `control_tower` (13 x 13 x 29 m). The real one is a circular glass-clad tower, 30 m across and 8-9 storeys (dossier "Buildings"). The position is OSM-derived and fine. The 141 OSM floodlight masts are laid as 136 `floodlight_tower`s (ok, night race). |
| Stands | ok | All the stands the maps show are laid from OSM: Main, First Turn, University 1-3, Batelco (infield, 3010 R, facing the back straight), Victory 1-2. The VIP tent between Turn 1 and the Main Grandstand (gpgt) is missing: a dossier `stage`/tent entry with `marquee_peak_12m`, minor. The GA areas (hatched) are People. The giant screens (gpgt: 7) are not laid; they are dossier `screen` landmarks, listed below. |
| Trees | ok | The SAKHIR style lays 208 trees in sparse palm clumps (palm_oil, palm_ornamental, bush). The dossier says "No lakes or trees of note (paddock is landscaped oasis)". `palm_oil` is a plantation palm; date palms would be truer (minor; `palm_ornamental` covers it). |
| Barriers | ok | Armco 1724, tecpro 1110, tyres 239, fence 212 behind the stands. That fits a Tilke circuit with wide asphalt run-off. Of the 71 mapped walls, none comes out as `concrete_4m_rail`, so they lie off the road (perimeter). Not checked per cell. |
| Boards | dossier | `corners` holds only "Grand Prix Circuit", so no corner signs are laid and only 12 marshal posts (every 400 m). T1 needs "Michael Schumacher" and T2-T15 their numbers. 473 hoardings, 19 braking boards and the car parks are fine. |
| People | kit | GA banks before T4 (left) and on the T12-T13 run (right) (gpgt hatching), a fan village behind the Main Grandstand, marshals at posts. Wanted: GA crowds on sand banks, crowd in front of the fan stage, marshals. |

## Positions from the maps

| Structure | Should be | Dossier today | Map |
|---|---|---|---|
| S/F line vs Main Grandstand | the line falls inside the Main Grandstand's span (34 % along on gpgt, 60 % on the seating chart) | the S/F marking (station 0) is at the stand's upstream end: OSM front 25-374 | gpgt, oversteer48 seating |
| Podium | right (pit side), at the entry (upstream) end of the garages, about -150..-100 relative to the line | middle garage (generated) | oversteer48 seating |
| Pit-building TVs (3) | right, on the pit building facing the Main Grandstand, spread over its length (about 15 %, 50 %, 85 % of the stand) | none | oversteer48 seating |
| Giant screen, T1 | right/inside of T1, about 700 | none | gpgt |
| Giant screen, University | right (inside), opposite University 2, about 1150 | none | gpgt |
| Giant screen, Batelco | infield, on the back straight's inside, in front of Batelco, about 3000 | none | gpgt |
| Giant screens, S/F | right, beside the Paddock Club, either side of the line (about -60, 0, +100) | none | gpgt |
| VIP tent | left, between Turn 1 GS and Main GS, about 430-470, about 30 m | none | gpgt |
| GA before T4 | left, about 1300-1480 | none (People) | gpgt |
| GA at T12-T13 | right (outside), about 3900-4020 | none (People) | gpgt |

## Kit gaps

- `building/tent_roof_pit_block` / `grandstand/bay_10m_tent_roof`: the white
  PTFE tent-peak roofs on the pit building and the main grandstand.
- `building/round_glass_tower` (30 m across, about 35 m tall, glass drum):
  the Sakhir Tower.
- `tree/date_palm` (optional; Gulf circuits; also YasMarinara).
- People family.

## Rule gaps

- **Podium position:** `ue_export` always seats `building/podium` on the
  middle garage. Bahrain's stands at the entry end of the garages. This
  wants a dossier-driven podium station, or a `podium` landmark kind.

## Surprising

- The game's S/F marking sits at the upstream end of the OSM Main
  Grandstand. Both maps put the line well inside the stand. Either the
  track's station 0 is about 100-200 m early, or the OSM outline is shifted.
  Check this before the S/F pass.

## Addendum: FIA PDF rendered (fia-2024-bahrain-circuit-map-red-zone.pdf p.2)

- The PDF marks a **Start Line** and a separate **Control Line** (the
  chequer, at marshal post M40). The control line is about 40 % of the way
  from T15 to T1, well inside the Main Grandstand's span. This backs the
  "Surprising" note: the game's S/F at the stand's upstream end is early.
- **Marshal posts:** the PDF numbers about 40 of them (M0.7 .. M40, plus
  18 FIA marshal light panels). The scene has 12 marshal posts and 19
  light panels. This strengthens the Boards cell: corners T1-T15 in the
  dossier would give a post per corner. Better still would be a dossier
  marshal-post list taken from the FIA map.
- The corner directions and order match the curvature frame above.
## S/F dossier pass

In the tables:

- `STAND_COVER`: Main Grandstand covered.
- `MANUAL_LANDMARKS`: screen at 95.0 right; screen at 205.0 right; screen at 305.0 right.

Left, or for later columns:

- Station 0 is early: both maps and the FIA red-zone PDF (control line at M40, about 40 % from T15 to T1) put the line well inside the Main Grandstand (34 % on gpgt, 60 % on the seating chart, about 145-235 here), but the YAML's S/F is at the stand's upstream end (front 26-375). A track-geometry question upstream of dressing; the screens are placed against the stand, not the line, so they hold if station 0 moves.
- Kit gap (the cell's main need): the PTFE tent-peak roofs on the pit building and the Main Grandstand (building/tent_roof_pit_block, grandstand/bay_10m_tent_roof). Covered means bay_10m_large_roof today.
- Rule gap: the podium belongs at the entry (upstream) end of the garages (the seating chart draws it off the stand's upstream end, about 0-30 here); the export always seats it on the middle garage.
- The VIP tent between the First Turn and Main grandstands (gpgt; left, about 440-476, about 30 m) is on the straight, but no landmark kind lays a marquee. As MANUAL_FURNITURE it would be, unchecked against a dress run: dict(kind="attraction", asset="marquee_peak_12m", from_m=440, to_m=476, every_m=14, side="left", offset_m=20),
- The screens are good to about 50 m and stand on the ground behind the garages, not on the pit-building roof (rule gap, as at AlbertParkour). The T1, University and Batelco screens are Stands-column work.
- First Turn Grandstand (OSM, 540-725 left) is laid uncovered; no map or dossier line says it is roofed, so it is left alone.

## Landmark dossier pass

In the tables:

- `MANUAL_LANDMARKS`: the Sakhir Tower is the round glass tower: `glass_tower`, on the OSM outline's centre (85.8 m off; the old offset was the outline's nearest corner).

Left, or for later columns:

- The tent-fabric roofs are built (`tent_roof_10m`, `garage_roof_membrane_6m`) and belong to the Stands column and the pit rule.

## Stands dossier pass

In the tables:

- The main grandstand is `tent` (the membrane roofs). Two general-admission banks on the left, before T4 and after T13.

Left, or for later columns:

- The other stands' shade roofs are unsourced; the map is not to scale, so the banks are anchored on the OSM stands beside them.

## Trees dossier pass

In the tables and rules:

- The desert set plants date palms (`date_palm_l`).
