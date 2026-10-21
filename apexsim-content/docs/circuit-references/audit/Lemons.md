# Lemons (Circuit de la Sarthe, Le Mans) dressing audit

Sources read: dossier `circuit-references/Lemons.md`; maps `racingcircuits-sarthe-15.png`
and `racingcircuits-bugatti-15.png` (corner names and the Bugatti loop only: no stands or
buildings on either). `aco-2026-plan-parkings.pdf` read from the coordinator's render
(`aco-2026-plan-parkings.pdf p.1`): a car-park plan that labels only
Tribunes (outside of the pit straight), Paddock and Village (both *inside*, the Village
beside the Dunlop curve by the Bugatti loop), Antarès, the airfield and the car parks
(Panorama, Expo, P3C PMR, Mulsanne, Arnage). No stand outlines. Stand positions below
therefore come from the dossier text and the OSM names in `Lemons.layout.json`. Explain
the `ats-dress --explain` output; MANUAL_STANDS / MANUAL_LANDMARKS / MANUAL_CROSSINGS / MANUAL_WOODS
`["Lemons"]` in `tools/osm_layout.py`; style DEFAULT.

Corner stations (layout corners): Courbe Dunlop 310-696, Chicane Dunlop 828-919, La Chapelle
1183-1244, Esses de la Forêt 1374-1699, Tertre Rouge 1699-2083, Hunaudières 2083-, Daytona
chicane 4051, Mulsanne ~7500, Indianapolis ~9000, Arnage 10156, Porsche 11467-11630, Pont
11722-12067, Karting 12067-12342, Corvette 12342-12581, Maison Blanche 12581-13190, Ford
13190-13322, Raccordement 13322-13443; lap 13618. Clockwise; pits right (inside), main
tribunes left.

## Cells

| Column | Cell | Reason |
|---|---|---|
| S/F | dossier | The pit straight's left tribunes 10-21 + T18 are OSM-traced and laid (13 stands, 1-2 bays each). But the pit lane is only 347 m (layout `pit_lane` 15 -> 365 m) where the dossier has the entry before the Ford chicane (~13190) and the exit at the Dunlop curve (~600), 60 garages: a `MANUAL_PIT_LANE["Lemons"]` (or reach) fix. Rule side: OSM's `Tribunes T34` (238 m, right, 25 m off, the stands over the pit building) is lost: explain "skipped stand Tribunes T34: nothing to build clear of the pit lane", plus the unnamed 311.8/349 right stands. Kit side: multi-storey pit building with hospitality above (dossier "Storeys: multi-storey with hospitality above"). |
| Landmark | ok | Tyre bridge authored at 1043 on the Dunlop crest (`bridge Tyre bridge: tyre_bridge`), fairground big wheel (`landmark (unnamed): ferris_wheel`, 1010 right), airship (`landmark Airship: blimp`), Passerelle Porsche footbridge (13000), the museum (M24, 6 x media_centre) and Maison Blanche are all laid. |
| Stands | dossier | The Dunlop/Chapelle/Esses tribunes are OSM buildings named "Tribune 4 - Pescarolo" ... "Tribune 9 - Goodyear", "Tribune T3 - Chapelle", "Tribune T1 - Tertre Rouge" and are laid as `clubhouse`/`media_centre` (explain `building Tribune ...`): promote them with MANUAL_STANDS entries of the same names (which replaces the structures). Karting stands T28-T32 (covered, big screen) missing. The Ford chicane / Raccordement stand ("large, covered") is OSM Tribune 23 at 74 m, laid open. MANUAL "Tribune Tertre Rouge" (1840-1990) and OSM "Tribune T1 - Tertre Rouge" (1596) may be the same stand twice. |
| Trees | rule | Woods are mapped well (40 rings, two of 1.6-4.5 km²; MANUAL_WOODS adds pine along 4700-8950 both sides), but `dress::forests` plants impostors only on DEM slopes and the card-tree belts stop at 90 m, so on flat Sarthe the big forests beyond the belt are bare (explain `surroundings: forest: 1 forest_impostor`). Rule: fill mapped woods beyond the belt with impostors regardless of slope. |
| Barriers | ok | Armco dominates (5138 + 759 fenced) as on the public-road sections, Tecpro 1694 at chicanes, tyres 83; the dossier gives no barrier detail. Note: no `concrete_4m_rail` anywhere (see rule gaps); vangrail/double-rail is not used outside the Nordschleife style, which the public-road sections (Hunaudières, Mulsanne-Arnage) may want. |
| Boards | rule | 549 hoardings, 38 marshal posts, 16 corner signs, 57 light panels are fine for the circuit part. Missing: the public-road furniture the dossier pitfalls call out ("Most of the lap is public road with roadside furniture, roundabouts (Mulsanne, Arnage) and junctions"): no `road_sign_post`, `km_marker` or roundabout dressing is laid outside the Nordschleife style. |
| People | kit | Huge camp-and-watch crowds (14 campsites with track views at Arnage, Beauséjour, Mulsanne, Tertre Rouge), spectator banks, marshals. The camp sites are laid (vehicles + tents), but the DEFAULT style has `fan_dressing` off, so no spectator banks along the camp/car-park fronts either. Wanted: standing crowd on banks/fences, campers by their tents, marshals at posts, a podium crowd on the straight. |

## Positions from the maps

The two readable maps carry only corner names, so nothing here is map-sourced; these are
the dossier/OSM positions the tables lack, for the trace page to confirm against the ACO plan.

| Structure | Where | Side | Extent | Source |
|---|---|---|---|---|
| Tribune 9 - Goodyear | Dunlop chicane exit, under the tyre bridge | left | OSM building at 907, 58 m | layout structures (OSM name) |
| Tribune 8 - Panorama | after the Dunlop chicane | left | OSM building at 996, 41 m | layout structures |
| Tribunes 7 - Rondeau, 6 - Bell, 5 - Ickx, 4 - Pescarolo | approach to La Chapelle | left (outside) | OSM buildings at 1127, 1164, 1195, 1221 (35 m each, i.e. ~1110-1240 as one run) | layout structures |
| Tribune T3 - Chapelle | La Chapelle exit | left | OSM building at 1266, 40 m | layout structures |
| Tribune T1 - Tertre Rouge | Esses exit / Tertre Rouge entry | left | OSM building at 1596, 35 m (check against MANUAL 1840-1990) | layout structures |
| Dunlop public stands + giant screen | Courbe Dunlop | outside (left) | dossier: "two free public stands + giant screen"; likely Tribunes 8-9 above; screen near ~600-900 | dossier "Buildings and structures" |
| Tribunes T28, T29, T30 | Karting corner, Maison Blanche side | outside | ~12100-12350 | dossier (ACO grandstand page) |
| Tribunes T31, T32 (covered) | Karting fan zone | outside | ~12200-12350 | dossier |
| Karting spectator mound (2025) | end of the Porsche Curves / Karting | inside | ~12000-12200 | dossier |
| Ford chicane grandstand (covered) | outside of Ford / Raccordement | left | OSM Tribune 23 at 13353 (74 m): mark covered, extend toward ~13200 | dossier, layout stands |
| Tribunes T34 (over the pits) | pit straight | right | 50-290 (OSM, 238 m, skipped) | layout stands, explain skip |
| Pit lane | entry before Ford chicane, exit at Courbe Dunlop | right | ~13190 -> 0 -> ~600 (now 15-365) | dossier "Start/finish, pit lane, paddock" |
| Village (shops, Welcome hall) | inside the Dunlop curve, between the paddock and the Bugatti loop | right (inside) | ~300-700, 50-200 m in; OSM "Welcome Center" at 389 right agrees. The dossier's "behind the grandstands on the outside" is contradicted by the ACO plan | aco-2026-plan-parkings (Village label) |

## Kit gaps

- `building/pit_building_tiered` with roof seating: Le Mans' multi-storey pit building with
  hospitality and the T34 tribunes on/over it (shared with Hungoverring, Interlaggos).
- Figures (People).

## Rule gaps

- A stand mapped over the generated garages (Tribunes T34) is dropped ("nothing to build clear
  of the pit lane") instead of being lifted onto the pit roof.
- `dress::forests`: mapped woods beyond the 90 m belt are not planted on flat ground.
- Public-road furniture (road signs, km posts, roundabout kerbs/islands) only exists for the
  Nordschleife style.
- `groom.rs:2326` builds every barrier `Cell` with `beside_pit_lane: false`, so the
  `barriers::decide` rule "the pit straight is separated by concrete" never fires on a real
  track: across all 28 explain files only Mordschleife has any `concrete_4m_rail`.
- `fan_dressing` (spectator banks along car parks / camp sites) is off for Le Mans, the venue
  it fits best after the Nordschleife.
## S/F dossier pass

In the tables:

- `STAND_COVER`: Tribune 23 - Motul Raccordement covered.

Left, or for later columns:

- No map places a screen on the pit straight, and the dossier gives only "some roofed" for the main tribunes ('category 3' stands unroofed), without saying which: no STAND_COVER for Tribunes 10-22.
- Tribunes T34 (OSM, 238 m, right, over the pit building) now lays 2 bays (current rule keeps the part clear of the garages); the rest is the rule/kit gap of stands on the pit roof (building/pit_building_tiered). Same for the two unnamed right-hand stands at 312 and 349.
- Pit lane: the layout lane is 15-365 m (347 m) where the dossier has the entry before the Ford chicane (~13190) and the exit at Courbe Dunlop (~600). A MANUAL_PIT_LANE["Lemons"] (or PIT_LANE_REACH_M) entry is the fix, outside the stand/screen tables; not drafted (moves the garages and every pit survey, needs its own check).
- Left for the Stands column: Dunlop public stands (Tribunes 8/9) and their giant screen (~600-900 left), Tribunes 4-9/T1/T3 promotion from buildings, Karting T28-T32, the Tertre Rouge duplicate.

## Landmark dossier pass

Nothing went in.

- Nothing needed: the tyre arch, the airship and the wheel are laid.
- The dossier puts the funfair near Maison Blanche, the tables inside the Esses; the ACO plan shows none: the wheel stays.

## Stands dossier pass

In the tables:

- Tribunes 9, 8, 7, 6, 5, 4, T3 and T1, which OSM maps as buildings, laid as stands under their OSM names; "Tribune Tertre Rouge" (a duplicate of T1, in the gravel) and "Tribune Virage Porsche" (no source) gone.

Left, or for later columns:

- Karting T28-T32 and the 2025 mound: no map places them.
- Mulsanne, Indianapolis and Arnage are kept but unverified.

## Trees dossier pass

In the tables and rules:

- Mapped woods past the belt fill with forest impostors from 120 m out, flat ground or not (R8). The two manual pine belts are marked authored, so a re-apply rebuilds them.
