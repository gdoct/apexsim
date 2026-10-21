# Snorisring audit (Norisring, Nuremberg)

Sources read: `circuit-references/Snorisring.md`, README row ("everything temporary except
the stone tribune"), maps `norisring-lageplan-2026.pdf` (rendered whole + east zoom),
`boxengassentribuene-draufsicht-2022.jpg`, `racingcircuits-norisring-1972.png`,
the `ats-dress --explain` output, `Snorisring.layout.json`, `.ats` by stamp. No MANUAL_* entry exists
for this stem (only its BBOX).

Frame: x east, y north. Lap 2279 m. The YAML/dossier has **no corners** (`corners: []`: the
lap is public roads with no named raceway ways). From the centerline headings:
S/F straight st ~1900 -> 2279/0 -> ~470 heading ESE; Grundig-Kehre (left hairpin) ~480-540;
return leg 550-900 heading WNW; S-Kurve / Schoeller-S (right-left jog round the
Steintribuene's east end) ~900-1000; back leg 1000-1580 behind the Steintribuene;
Dutzendteich-Kehre (left hairpin) ~1600-1690; T8 left kink ~1870-1900 onto the straight.
Pit lane right (south, Zeppelinfeld side), entry 1929, exit 70: matches the Lageplan.

## Cells

| Column | Cell | Reason |
|---|---|---|
| S/F | dossier | The Steintribuene faces the straight on the left (laid, 11 runs, OSM) and the pit lane is right, as on the Lageplan. Missing: the temporary stands round the straight (Boxengassen-, Milwaukee-, SPECK-Pumpen-Tribuene, none in OSM or MANUAL_STANDS), the DTM Fan-Zone/hospitality tents in the Zeppelinfeld, the start line ~170 m after the finish (Lageplan: FINISH by the tribune centre, START by Pre-start 2). Rule side: the Steintribuene is laid `bay_10m_large_roof` (depth 35.7 >= the 25 m always-roofed rule) but is an open stone terrace. Pit complex: temporary DTM-Pits with a lounge on top; the default style lays the permanent-garage kit (see rule gaps). |
| Landmark | kit | The landmark is the Steintribuene itself (1930s stone speakers' tribune, ~360 m, central platform block, no roof). No kit piece stands in: today it is roofed modern bays. Also: DEKRA fan tower (temporary, inside the Dutzendteich-Kehre, `tower`/`camera_tower` would do: dossier), the Grundig towers ("Turm 1", 16-storey slabs by the Grundig-Kehre) laid as 2 x `control_tower`, should be `skyline_slab_a` via MANUAL_STRUCTURE_ASSETS (dossier). |
| Stands | dossier | Only the Steintribuene and OSM's unnamed outlines are laid. The unnamed 187 x 84 m "stands" ~190-200 m right of st 0 and 2012 (11 x `bay_10m_large_roof` + 10 x `bay_10m`) are the Zeppelinfeld's stepped earth banks (Waelle), open terraces, not roofed stands; the 12.5 x 6 m ones skipped "too small to be seating" are its stone block towers. Add Milwaukee, Boxengassen, SPECK-Pumpen, THOMAS SOMMER, Alpha (positions below). |
| Trees | dossier | The Lageplan shows dense broadleaf park woods (Volkspark Dutzendteich) in belts between the back leg and the railway, round the lake and south of the Zeppelinfeld. The dossier has 3 woods (38,823 m^2 mixed at 1615 R 274 m out, 2,520 broadleaved at 791 R, 11,150 mixed at 1187 R); groom laid ~25 real trees and 2,184 grass/wildflower clumps. Add broadleaved woods (below). |
| Barriers | rule | Real: concrete walls and guard rails right at the road edge, no run-off (dossier "Modelling pitfalls"). `CircuitStyle::for_stem` gives Snorisring DEFAULT (corner barrier >= 14 m, straight >= 7 m, armco), so groom laid 692 armco + 148 armco_fence + 192 tecpro + 206 tyres and no concrete at all. Needs a temperate street style (STREET's 3/5 m and concrete, but temperate flora and tree belts kept). |
| Boards | dossier | No corners in the dossier means no `corner_sign`s and marshal posts only from the 400 m rule (5); one braking-board set only (3 boards). osm_layout.py has no MANUAL corners table, so adding Grundig-Kehre, Schoeller-S, Dutzendteich-Kehre needs one (tool change in the dossier tool). 234 hoardings fine. |
| People | kit | Crowds on the Steintribuene and the temporary stands, the fan zone/beer garden in the Zeppelinfeld, entrances K1/K2, marshals at the walls. No figure kit. |

## Positions from the maps

Lageplan scale along the S/F straight: map x 250 (hairpin exit) -> 1350 (Grundig) ~ st 1900 -> 480, ~0.78 m per px at 1.3x render.

- **SPECK-Pumpen-Tribuene**: right (south) of the straight just after the T8 kink, before Pre-start 1 / pit entry, small (~50 m). st ~1870-1925 R, depth ~10. (`norisring-lageplan-2026.pdf`)
- **Boxengassen-Tribuene**: right, behind the wall at the pit-lane entry stretch and the first grid boxes, ~70 m, four blocks, open scaffold bleachers (photo `boxengassentribuene-draufsicht-2022.jpg`). st ~1950-2020 R, depth ~18. Note: within the pit zone it may be dropped ("skipped stand (unnamed): 2 prop(s) in the pit lane"); give it a gap past the garage depth or place it before the garages start.
- **DTM-Pits** (temporary garages, lounge on top): right, st ~2190-2279/0-30. Export's.
- **Milwaukee-Tribuene**: right, after START between Parc Ferme and Hans-Kalb-Strasse, st ~100-215 R, depth ~12.
- **Start line**: ~st 60 (by Pre-start 2), finish ~st 2175 (under the Steintribuene's centre). Two lines.
- **THOMAS SOMMER-Tribuene**: north of the return leg, inside the elbow of the S-Kurve's first (right) part, angled to face it: st ~860-940 R, depth ~12.
- **Alpha-Tribuene**: north of the return leg just after the Grundig-Kehre, st ~560-660 R, depth ~16, open.
- **Steintribuene**: left of the S/F, OSM outline ok (1980-2279+). Open, no roof; central platform block projecting; 2026 middle section closed.
- **DEKRA Tower** (10): inside the Dutzendteich-Kehre, between the back leg and the Steintribuene's west end: st ~1520-1580 L (left of the back leg), ~30 m out. Landmark `tower` (or `camera_tower`).
- **Grundig towers** ("Turm 1", 16 levels, two tall slabs): north-east by the Grundig-Kehre beyond the railway, st ~530 R 27 m (OSM). Asset `skyline_slab_a` / `skyline_slab_b` through MANUAL_STRUCTURE_ASSETS.
- **Zeppelinfeld** (south of the straight): stepped banks with stone block towers round a field with an oval track; hospitality and DTM Fan-Zone tents in the field's north half (st ~1990-2150 R, 40-150 m out). `tent_6m` / `food_stall_6m`, `fanzone_stage` + `video_screen` (dossier: big screen in the fan zone).
- **Pedestrian crossings**: yellow dotted lines across the back leg at the S-Kurve (st ~950) and across the straight at START (~60); ADAC pedestrian tunnel under the straight at the Steintribuene's west part (~2000). Nothing tall; skip.
- **Woods** (broadleaved): belt north of the return/back leg between track and railway st ~600-1550 R, 15-60 m out; round the Dutzendteich lake west of the hairpin (st ~1600-1700 R/outside, 40-200 m); south-west of the Zeppelinfeld (st ~1850-1950 R, 150-300 m); south and east of the Grundig-Kehre (st ~420-560 R, 30-120 m).

## Kit gaps

- `building/stone_tribune` (a long open stone terrace with a central speakers' platform block, no roof), 10 m tiled + centre piece: the Steintribuene.
- `building/stone_block_tower` (small stone-clad block tower on an earth bank): the Zeppelinfeld's 34 towers (optional).
- People figures as everywhere.

## Rule gaps

- **Always-roofed at 25 m depth** (`ASSUMED_ROOF_DEPTH_M`) roofs open terraces: the Steintribuene and the Zeppelinfeld banks. Needs an explicit "open" in the dossier (tri-state `covered`) or a terrace family (`banking_seats`).
- **No temperate street style**: `CircuitStyle::for_stem` sends Snorisring to DEFAULT; STREET is tropical, treeless and paved. A `TEMPORARY_STREET`-like style: barriers at 3/5 m, concrete with fence, temperate trees, temporary pit garages.
- **No manual corners** in osm_layout.py for circuits on public roads (no corner signs, no named marshal posts).
## S/F dossier pass

In the tables:

- `MANUAL_STANDS`: SPECK-Pumpen-Tribüne (1850-1910 right); Boxengassen-Tribüne (1990-2055 right); Milwaukee-Tribüne (95-185 right).
- `STAND_COVER`: Steintribüne open; Boxengassen-Tribüne open.
- `MANUAL_LANDMARKS`: screen at 1924 left; screen at 64 left.

Left, or for later columns:

- The screen symbols are read as giant screens (the legend does not name the camera-board icon); the same symbol stands at the Dutzendteich- Kehre (~1825 L) and the Grundig-Kehre (~420 R), left to Stands.
- Start and finish are two lines (FINISH under the tribune centre ~2175, START ~60 by Pre-start 2): not a dossier table; the export lays one.
- DTM-Pits (temporary garages, lounge on top) are the export's pit complex; the default style lays permanent garages (rule gap).
- The unnamed OSM "stand" at st 1962 R (166 x 34 m, running south from 21 m off the road, perpendicular to it) and the two 187 x 84 m ones ~200 m out are the Zeppelinfeld's earth banks; unnamed, so STAND_COVER cannot open them (they are roofed by the depth rule). Needs a bank family or a cover-by-position key.
- DTM Fan-Zone big screen (dossier) sits ~165 m out in the field (st ~2036 R): left out, not "by the straight"; Thomas Sommer and Alpha Tribüne are on the return leg (Stands column).
- Steintribüne as a stone terrace with a central platform is a kit gap (building/stone_tribune).
