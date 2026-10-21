# Armadillos (Circuit of the Americas, Austin) - dressing audit

Sources: dossier `circuit-references/Armadillos.md`; maps
`oversteer48-grandstand-map-2025.jpg` and `f1-programme-usa-circuit-2025.webp`
(both read as images). `fia-2024-circuit-map-pit-lane.pdf` could not be
rendered: no poppler/pdf renderer on this machine, and the file has no
plain embedded raster stream. Also the `ats-dress --explain` output,
`Armadillos.layout.json`, `osm_layout.py` MANUAL_STANDS "Armadillos" (Turn 4,
Turn 9) and CORNER_DISPLAY "Armadillos".

Station frame: the dossier's own `corners` (station, from-to). They are
T1 655 (569-715), T2 903, T3 1171, T4 1260, T5 1355, T6 1531 (1408-1666),
T7 1724, T8 1891, T9 1978, T10 2177, T11 about 2530-2620 (it is not in the
dossier: the corner entry is OSM's "Short Course 2" at 3416, which is the
short-course junction, not T11), T12 3778, T13 4003, T14 4091, T15 4213,
T16 4286, T17 4538, T18 4748, T19 5049, T20 5337. The lap is 5490 m. The
S/F marking is at station 0, about 15 % down the straight from T20. That
matches the F1 programme map's chequer and sector line at the T20 end. The
oversteer48 map instead draws the chequer at the T1 end of the Main
Grandstand, about 350 m further on. COTA has separate start and finish
lines, so this is not counted as an error.

## Cells

| Column | Cell | Reason |
|---|---|---|
| S/F | dossier | The pit lane (950 m, left, from inside T20 to near T1) and the generated garages are right. The Main Grandstand is OSM's unnamed stand at 102.8 R, 312 m, depth 15.2, `covered: False`. It is laid as plain `bay_10m_large`, but the real stand is a 3-level, 20 m stand under a fabric canopy (dossier "Main Grandstand"), so it needs a name, covered=True and the stadium family (`bay_10m_stadium_roof`). The "four giant TVs on the pit building" facing it (dossier S/F) are not laid: no `screen` landmarks. |
| Landmark | rule | The observation tower is found by OSM (landmark `tower` at 4654.7 R, the inside of the T16-T18 carousel: right position). But `lay_landmark` maps `tower` to `control_tower`, and explain shows "landmark (unnamed): 1 x control_tower". The kit already has `building/observation_tower` (8.3 x 8.3 x 60.5), which no landmark kind reaches. It needs a kind such as `observation_tower`, plus a `LANDMARK_KINDS["Armadillos"] = {"tower": "observation_tower"}` dossier line. The amphitheatre at its base comes out as one `media_centre` box (kit: stage canopy). |
| Stands | dossier | The OSM stands at T1, T12, T15 and T19-T20 and MANUAL Turn 4 / Turn 9 are laid. The maps also show the **Turn 2**, **Turn 6** and **Turn 13** stands, which are missing. T19 is covered in the dossier but its OSM stands are uncovered. All the OSM stands are unnamed. |
| Trees | rule | `woods 0`. 6482 groom trees are the DEFAULT temperate belt (poplar 341, conifer 322, broadleaf 591), packed 3-6 per cell in a continuous 22-90 m band. COTA stands in open Texas prairie and car parks ("mostly open Texas scrub and fields", dossier "Public roads"). It needs a sparse open-country style (like SAKHIR's clumps) with live oak / juniper, not a continuous belt. |
| Barriers | ok | Mapped `guard_rail` (52 runs) drives the armco; there are 1303 tecpro on the fast outsides and 205 armco_4m_fence. That matches a modern permanent circuit with big asphalt run-off. It has not been checked cell by cell: confirm that the main straight has concrete + catch fence on both sides. |
| Boards | ok | 455 hoardings, 16 braking boards, 19 corner signs from CORNER_DISPLAY, 25 marshal posts, 12 named lots of car parks and the RV park as a camp. The dossier's tall red Turn 11 marker is not represented (minor). The same rule gap as Albert Park applies: 23 generated `floodlight_tower`s at a day venue with no track lighting. |
| People | kit | Large GA hills (T1, T3, T6, T9 crest, T16 tower base), Grand Plaza fan zone, marshal posts. Wanted: GA lawn crowds on slopes, standing crowd at the fence, fan-zone crowds, marshals. |

## Positions from the maps

All are left/right in the driving direction.

| Structure | Should be | Dossier today | Map |
|---|---|---|---|
| Turn 2 Grandstand | right (inside of the right-hander T2), about 870-960, small | missing | oversteer48-grandstand-map-2025 |
| Turn 6 Grandstand | left (outside of the right-hander T6, along the start of the 6->7 run), about 1550-1680, small | missing | oversteer48-grandstand-map-2025 |
| Turn 13 Grandstand | left (on the T13->T14 chute's south side), about 4000-4090, small | missing | oversteer48-grandstand-map-2025 |
| Turn 15 (two stands) | right: one at the T15 apex, one long along T15-T16 | laid (OSM 4256/4289/4356 R) | oversteer48 (ok) |
| Turn 19 Grandstand | right (outside), T18->T19, covered | laid uncovered (OSM 4940/5020 R) | oversteer48 + dossier "covered" |
| Main Grandstand | right, about T20 exit (5450) -> about 350, canopy | OSM 0-310 R, uncovered | oversteer48 |
| Giant screens | four on the pit building (left, about 0-300, facing right); one infield across T1 (left, about 650); one before the T16 bridge (about 4250) | none | dossier text (not on the maps) |
| Footbridges | T2/T3 (1134) and T16 (4485) as in the dossier; a third OSM truss at 2159.5 (T10) is not in the dossier | 3 truss_bridge laid | dossier text vs explain |
| Observation tower | right, inside of T16-T18, about 4650, about 77 m | position ok, wrong asset | dossier + OSM |

## Kit gaps

- `attraction/amphitheatre_canopy` (a stage under a large white tensile
  roof): the amphitheatre at the tower's base. Optional; the media_centre
  box stands in.
- People family.

## Rule gaps

- **No landmark kind reaches `building/observation_tower`**: `tower` is
  always `control_tower`. Add an `observation_tower` kind (or a per-stem
  `tower` override).
- **Open-country tree style**: the DEFAULT belt is a continuous temperate
  forest band. COTA, and others in prairie or farmland, need sparse clumps.
- **Floodlights at day venues** (as at AlbertParkour): DEFAULT
  `floodlights: true` lays a ring wherever the dossier maps no masts.

## Surprising

- The dossier has no T11 corner (the T11 slot holds OSM's "Short Course 2"
  junction at 3416, and display None), so T11, the hairpin at about 2575,
  gets no corner sign or named marshal post.

## Addendum: FIA PDF rendered (fia-2024-circuit-map-pit-lane.pdf p.3)

- The pit lane drawing confirms the lane on the inside (left) of the
  straight. The entry branches at T20 and the SC1 line is at the T20 apex.
  The lane runs past the 34 garages (FIA/FOM boxes at both ends) and
  "Pit Lane Ends" before the SC2 line, which sits just short of T1. Pole is
  on the RHS. This matches the dossier lane (950 m, left), so S/F stays
  dossier only for the Main Grandstand roof and the screens.
- **Generated box count:** the drawing has 34 garages; the generated
  minimum is 24 boxes (`PIT_GRID_BOXES`). Check the generated count on the
  950 m lane.
## S/F dossier pass

In the tables:

- `MANUAL_LANDMARKS`: screen at 20.0 left; screen at 105.0 left; screen at 190.0 left; screen at 275.0 left.

Left, or for later columns:

- The Main Grandstand is in the right place: OSM's unnamed stand runs about 0-311 right, 19 m off; oversteer48-grandstand-map-2025 has it from just after T20 (about 5460) to about 350, within 50 m. It is laid uncovered (covered False, depth 15.2), but the real stand is 3 levels, about 20 m tall, under a fabric canopy. Neither table can reach it: STAND_COVER keys by name and the OSM stand has none, and a MANUAL_STANDS "Main Grandstand" would lay on top of it (name promotion drops OSM buildings, not OSM stands). Rule gap: a way to name or roof an unnamed OSM stand (by station and side), then STAND_COVER {"Main Grandstand": "covered"}. No stand family reaches bay_10m_stadium_roof (only the street families are picked by `family`): a "stadium" family is a second rule gap; bay_10m_large_roof is the best the kit gives today.
- The trackside bleachers flanking it (3 blocks west, 6 east; dossier) are on no map: not placed.
- Pit lane (950 m, left) and garages match the FIA pit-lane drawing; the generated 24-box minimum against the drawing's 34 garages is an export check, not a dossier one.
- Screen positions are to about 50 m (text only). The infield screen across T1 (left, about 650) and the one before the T16 bridge are Stands-column work; the digital leaderboard at pit exit has no map position and is not placed.
- S/F marking: station 0 matches the F1 programme map's chequer; the oversteer48 chequer at the T1 end of the stand is COTA's separate start line, not an error.
- The Main Grandstand, OSM's unnamed outline at 102.8 m right, is roofed by its station (`STAND_COVER` "@102.8"); the stadium family it really has is a rule gap (R13).

## Landmark dossier pass

In the tables:

- `LANDMARK_KINDS`: OSM's tower at 4654.7 right is the observation tower (R11), laid as `observation_tower`.

Left, or for later columns:

- The amphitheatre canopy: a smaller kit ask.
