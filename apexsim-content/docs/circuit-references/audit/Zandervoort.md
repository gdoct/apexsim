# Zandervoort audit (Circuit Zandvoort)

Sources read: `circuit-references/Zandervoort.md`, README row ("old 1999-2019 layout"),
maps `2026-dutch-gp-wayfinding-positive.pdf` (rendered whole and in three zooms),
the `ats-dress --explain` output, `Zandervoort.layout.json`, `MANUAL_STANDS["Zandervoort"]`
(no MANUAL_LANDMARKS / CROSSINGS / WOODS / STRUCTURES entries), the `.ats` props by stamp.

Frame: x east, y north. Lap 4313 m clockwise. Main straight st 4103-348 heading NNE;
pit lane on the right (east). Corners (YAML): Tarzan 414 (348-476), Gerlach 735,
Hugenholtz 916, Hunserug 1152, Slotemaker 1426, Scheivlak 1787, Masters 2140,
T9 (unnamed in YAML) ~2300-2400, T10 "CM.com bocht" 2578 (2428-2666), back straight
2666-3167 westbound, Hans Ernst 3247 (3167-3320), T12-T13 run 3320-3650 southbound,
T13 ~3700, Arie Luyendyk 3850 (3722-3990).

## Cells

| Column | Cell | Reason |
|---|---|---|
| S/F | dossier | Pit building is the export's (explain: "skipped building Pitgebouw: in the pit complex"), main stands 28/27 are laid (`stand Hoofdtribune` 5-45 L, `stand Pit Grandstand` 90-215 L). Missing: Tarzan-In 26 (left out on purpose, see below), Pit Grandstand 27 is the only covered stand (dossier "Buildings") but is laid as `bay_10m_large` without roof; the many LIVE SCREENs along the pit straight (map) and the hospitality lounges 15-19 behind the west stands are absent. |
| Landmark | dossier | Ferris wheel west of the straight's south end by Ben Pon 29, and the F1 Fanzone stage SW (dossier "Buildings", map icons): `layout.landmarks` is empty and explain lays 0 landmarks. Both have kit (`big_wheel` -> ferris_wheel, `stage` -> fanzone_stage). |
| Stands | dossier | MANUAL_STANDS places "Hairpin" at 3150-3330 R, which is the Hans Ernst chicane (ats: bays at 3205-3314 R); the real Hairpin Grandstands 34 are inside the T9-T10 loop (map). Eastside 1-3 (33) along the south of the back straight are missing; "Eastside" is laid at 3349-3783 L (that is Eastside 4 / 35, far too long, runs into T13); "Arena" 3300-3760 R lumps Arena 31 and Arena-Out 30 and runs to T13. Arena-In 32 missing. Rule side: these are temporary scaffold stands and GA1/GA2 dune banks have no family (see rule gaps). |
| Trees | rule | The dunes are scrub and marram grass with pine stands. Groom laid 3196 "trees" of which 3169 are grass/wildflower clumps and only ~27 trees; the dossier carries 49 `scrub` areas (several >20,000 m^2 right beside the lap, e.g. 93,202 m^2 near st 3643 L, 11,222 m^2 at 1164 L) that no pass plants. Dossier also: the map's "Wooded Area" south of the back straight between the lakes has no wood (only 3 small `mixed` woods). |
| Barriers | dossier | Groom: 1484 armco_4m_fence, 298 armco, 600 tecpro, 55 tyres (at Masters 2028-2044 L, 3344-3404 L, 3593-3673 L). The dossier says Tarzan's run-off and escape road run "along the tyre barrier"; nothing tyre-like is laid at Tarzan's outside (OSM has only guard_rail nodes at 385-480 L, 22-55 m out). Add a `tyres` barrier line there. Old layout: no SAFER at T14, correct as is. |
| Boards | rule | 489 hoardings, 10 corner signs, 13 marshal posts, 18 light panels are fine. Distance boards: only two sets (145-295 L for Tarzan; 3349-3499 L, which is 150 m short of T13). None for Hans Ernst (heavy braking, 3167) or T9/T10. Check the braking-corner detection / skip-on-overlap in `groom::lay_distance_boards` (the misplaced Hairpin stand sits right in the Hans Ernst approach). |
| People | kit | GA1 dune banks outside T2-T9 and GA2 inside T13-T14, the orange crowds on every stand, gates 1/2/4, marshals: no figure kit. Wanted: standing/sitting crowd on a slope (dune bank), crowd at gates and fanzone, marshal figures at posts. |

## Positions from the maps (`2026-dutch-gp-wayfinding-positive.pdf`)

The map is the current layout; the route between corners is the same in game.

- **26 Tarzan-In Grandstands**: west (left) of the straight's north end, *before* the braking zone, right next to the road, short (~60-80 m). Station ~250-330 L, gap ~10 m. The MANUAL comment drops it because "run-off and gravel reach 35 m on the outside" of Tarzan; on the map it is on the straight, not in Tarzan's outside. Try from_m 250, to_m 330, side left, depth 14.
- **27 Pit Grandstand**: laid right (90-215 L); set `covered=True` (dossier: only covered stand).
- **28 Main Grandstand**: = OSM Hoofdtribune (5-45 L), at the chequered flag. OK.
- **29 Ben Pon**: 4030-4240 L. OK.
- **34 Hairpin Grandstands (two blocks)**: inside the T9-T10 hairpin, between the T9->T10 run and the start of the back straight, both blocks parallel to the T9-T10 run. Station ~2450-2600, side left (inside of the left-hand T10; north of the eastbound run), depth ~14. Replace the current "Hairpin" 3150-3330 R.
- **33 Eastside Grandstands 1-3 (three blocks)**: south (left) of the back straight: one block just west of the infield lake (~2800-2880 L), one at the Hans Ernst entry (~3080-3170 L), one south of the chicane facing T11-T12 (~3200-3290 L, angled). Depth ~14-16.
- **32 Arena-In**: north (right) of the back straight approaching T11, ~3060-3160 R, depth ~12.
- **31 Arena Grandstands (several blocks)**: north of Hans Ernst Bocht 1 (inside the right-hander, ~3180-3260 R) and west of the T12-T13 run (~3330-3460 R).
- **30 Arena-Out**: west (right) of the T12-T13 run, south of the 31 block, ~3480-3560 R. So "Arena" should end ~3560, not 3760.
- **35 Eastside Grandstand 4**: east (left) of the T12-T13 run, opposite 31/30, ~3400-3470 L, short. So "Eastside" 3330-3800 L should shrink to that.
- **GA1** (purple): continuous bank on the outside from Gerlach/T2 (st ~650 L) past Hugenholtz, Hunserug, Slotemaker, Bocht 6, Scheivlak, Mastersbocht to T9 (~2350), i.e. left of the lap from ~650 to ~1700 (north side) and outside of Scheivlak/Masters. Dune bank, no seats.
- **GA2**: inside T13-T14 (right), ~3700-3850 R, a bank south of the paddock.
- **Ferris wheel**: west of the straight's south end, beside Ben Pon 29 and the small fanzone square: ~st 4200-4280 L, ~70-90 m out. Landmark kind `big_wheel`.
- **Fanzone stage + Fanzone** (red/black hatch): outside the lap south-west of T14 / start of straight, ~st 3850-4000 L, 120-250 m out. Landmark `stage`; tents/food stalls.
- **LIVE SCREENs** (landmark `screen`): inside the pit straight at ~st 4250 R (by the media centre), ~100 R and ~250 R (on the paddock club row), outside Tarzan ~430 L (north-east corner), Gerlach ~700 R, Hugenholtz inside ~900, Hunserug ~1200 R (by 21), Bocht 6/Scheivlak ~1650 R, T10 ~2600 R, T9 ~2450 R, back straight ~3000 R (by 32), Hans Ernst ~3230 L, T12-T13 run ~3500 R and ~3560 L, T14 ~3850 R. Pick the five or six biggest (pit straight, Tarzan, T10, Hans Ernst, T14).
- **F1 Paddock Club (red)**: a long block inside the pit straight behind the pit building, plus a separate block inside Tarzan's apex (~st 330-420 R, 30-50 m out). Temporary hospitality; `hospitality_3f` as MANUAL_STRUCTURES.
- **Hospitality lounges 15-19 + Champions Club**: a row of orange blocks behind (west of) stands 26-29 along the N200 side, ~st 4150 -> 330, 50-80 m left. `hospitality_3f` / `clubhouse`.
- **Media centre (purple hatch)**: south end of the pit building (~st 4230 R). Inside the pit complex zone; part of the export's building.
- **Wooded area** (tree icon): south of the Hans Ernst / T13 area between the two big lakes, ~st 3300-3600 L, 150-250 m out. Wood, leaf needleleaved (dune pine).
- **Campsite (538 Village)**: south-east beyond the lakes, outside Masters/T9; OSM's camp sites are elsewhere (De Duinrand at st ~110 L, 175 m out). Event-only; optional.
- **Track tunnels**: crossing marks across the straight south of Ben Pon (~st 4000) and across T13-T14 (~3780). Underpasses only, nothing visible above road except portals; low priority.

## Kit gaps

- People: `misc/crowd_bank_10m` (standing crowd on a slope, for GA dune banks), `misc/crowd_group` (gates, fanzone), `misc/marshal_figure` (posts). Cross-track.

## Rule gaps

- No stand family for **general-admission banks**: a MANUAL_STANDS entry cannot lay `misc/spectator_bank_10m` (or `grandstand/banking_seats`); `surroundings::spectator_banks` only lines car parks and camp sites. GA1/GA2 need `family="bank"` (or a dossier `spectator_areas` list).
- No **scaffold** family for temporary stands: `stand_family` only picks bay/street families; `grandstand/scaffold_10m` exists in the kit. Most Zandvoort stands are temporary (~85,000 temporary seats).
- **Scrub areas are never planted**: OSM `scrub` areas (49 here) get no `bush_cluster` / `scrub_clump`; dunes read as lawn.
- **Distance boards** missing at Hans Ernst and T9/T10 (see Boards).
- Oddity: the YAML corner "CM.com Circuit Zandvoort" at st 712 (display None, a way named after the venue), and T9/T13 have no named corner, so marshal posts/corner signs at those come only from the 400 m rule.
## S/F dossier pass

In the tables:

- `MANUAL_STANDS`: Tarzan-In Grandstands (265-335 left).
- `STAND_COVER`: Pit Grandstand covered; Hoofdtribune open.
- `MANUAL_LANDMARKS`: screen at 320 right; screen at 4260 right.

Left, or for later columns:

- The north screen is drawn at ~st 300 R; at 290 R / 25 m it was skipped ("in the pit lane or where its garages stand"), so it is 30 m further on, past the garage row.
- Hoofdtribune open follows the dossier's "only covered stand" line; no photo of the 2020 main stand was checked. Doubtful: confirm.
- Hospitality lounges 15-19 and the Champions Club behind 26-29 (~4150 -> 330, 50-80 m L) and the Paddock Club blocks are structures (MANUAL_STRUCTURES), not these tables; the other live screens round the lap are the Stands column's.

## Landmark dossier pass

In the tables:

- `MANUAL_LANDMARKS`: the Ferris wheel (4135 left, 70 m, behind Ben Pon 29) and the F1 Fanzone stage (3850 left, 220 m), 2026 wayfinding map.

Left, or for later columns:

- The wheel stays south of about 4200, clear of the OSM lounge building at 4233 left.

## Stands dossier pass

In the tables:

- "Hairpin" (at Hans Ernst) and the lumped "Arena" and "Eastside" gone; the two Hairpin 34 blocks inside T9-T10, Eastside 33, Arena-In 32, Arena 31, Arena-Out 30 and Eastside 4 (35) added, the small Bronze blocks as `scaffold`; the GA2 bank and GA1 in six pieces from Tarzan's exit to Masters.

Left, or for later columns:

- GA1 round Bocht 9 (the map puts it in the layout's lake); the live screens.

## Trees dossier pass

In the tables and rules:

- The 49 dune-scrub areas grow bushes (R8).

## Boards dossier pass

- `MANUAL_CORNERS` (R10): Bocht 6, 9 and 13 added; OSM's circuit way, which was taken for a corner, dropped.
- Marshal posts every 150 m along the straights (the rule).
