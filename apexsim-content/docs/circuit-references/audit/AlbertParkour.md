# AlbertParkour (Albert Park, Melbourne) - dressing audit

Sources: dossier `circuit-references/AlbertParkour.md`; maps
`oversteer48-gates-entrances-map.jpg`, `oversteer48-fangio-grandstand-map-2026.jpg`
(both read as images); the `ats-dress --explain` output; `AlbertParkour.layout.json`;
`tools/osm_layout.py` (MANUAL_STANDS "AlbertParkour", MANUAL_PIT_LANE,
MANUAL_STRUCTURES, DAY_RACE_NO_FLOODLIGHTS, NO_OSM_STANDS).

Station frame used below (no named corners in the YAML or dossier,
`corners: 0`; read off the centerline's curvature, S/F at 0, lap 5286 m,
clockwise): T1 R 340-400, T2 L 420-540, T3 R 1060-1140, T4 L 1210-1280,
T5 R 1430-1490, T6 R 1860-1910, T7 L 1950-2010, T8 R 2080-2260,
**R-L chicane 2520-2670 (the old Clark chicane, removed in 2021)**,
T9 L 3320-3380, T10 R 3450-3500, T11 R 4120-4190, T12 R 4370-4470,
T13 L 4630-4710, T14 R 4760-4870. The guide (`AlbertParkour.guide.yml`)
also lists 16 corners, including a "slow right-left" at 2558.

## Cells

| Column | Cell | Reason |
|---|---|---|
| S/F | dossier | `MANUAL_PIT_LANE` runs 0-380 m on the right, so the lane, the generated garages and the podium sit between the line and T1. Both maps show the lane leaving at T13/T14 (about 4760) and rejoining about 200 m before T1 (about 140). The real garages face Senna, Fangio and Piastri, with the support garages at the entry end. The main-straight stands are wrong too (see Stands), and the "two large screens on the pit building" (dossier, S/F section) are not laid: there are no landmarks. |
| Landmark | dossier | The venue's defining feature is **Albert Park Lake**, inside the lap, and the dossier has no water at all (`waterways 0`, and no `water` areas, because `WATER_STEMS` is only MandarinaBay). The temporary footbridges over the track (next to Fangio, at T8/Clark, at T11 Webber/Vettel; dossier "Buildings and structures") are missing: `crossings 0`, explain shows 0 bridges. Lakeside Stadium (MANUAL_STRUCTURES) is skipped with "reaches across the road". |
| Stands | dossier | 12 of 13 MANUAL_STANDS are laid (explain). Moss is skipped ("nothing to build clear of the pit lane") because it is entered as `side="right"`; both maps put it on the left. Waite, Webber, Lauda and Schumacher sit 800-1400 m early (they were counted on the 16-turn geometry). Ricciardo, Button, Vettel and the 2026 Piastri stand are missing. Senna is uncovered, but the dossier says it is covered (premium). |
| Trees | dossier | `woods 0`. 7661 groom trees come from the DEFAULT style's temperate belt (broadleaf, conifer, poplar, plus 5800 grass and wildflower clumps). With no lake in the dossier, the belt plants where the water should be. A city parkland of elms, gums and palms needs the lake first and then park belts; also kit (no eucalypt/gum tree asset). |
| Barriers | rule | The DEFAULT style lays armco at 7/14 m minimum (1498 armco_4m, 454 armco_4m_fence, 147 tecpro, 561 tires_4m). Albert Park is a temporary park-street circuit lined with concrete blocks and debris fence close to the road (guide overview: "lined with walls in many places"). It needs a style that lays concrete_4m_rail + catch_fence_6m close in (`circuit_style::for_stem` sends it to DEFAULT). |
| Boards | dossier | There are 649 hoardings and 19 braking boards, but `corners 0` and no CORNER_DISPLAY entry, so no corner signs are laid and only 13 marshal posts (every 400 m). The numbered corners need to go into the dossier. A rule gap is also laid here: 24 `floodlight_tower`s (explain "surroundings: floodlights") at a day race the dossier says has no lighting of its own. |
| People | kit | Large general-admission lawns behind the debris fence all round the lap, fan zone/Main Stage in the infield, marshal posts. Wanted: standing and sitting spectators on grass, spectators at the fence, marshals at posts, a crowd on footbridges. |

## Positions from the maps

All are left/right in the driving direction. Stations come from the
curvature frame above, with the map scale taken from the straight
(T14 to T1, about 1.9-2.0 m/px).

| Structure | Should be | Dossier today | Map |
|---|---|---|---|
| Pit lane / garages | right (lake side), about 4760 -> about 140 (entry at T13/T14, merge about 200 m before T1); support garages at the entry end, F1 garages opposite Fangio/Piastri | 0-380 right | fangio-2026, gates |
| Fangio | left, about 5050-5220 (from mid-straight up to the S/F line) | 30-330 left | fangio-2026 |
| Piastri (new 2026) | left, about 5240-90 (S/F line toward Moss) | missing | fangio-2026 |
| Moss | **left**, about 220-310, between Piastri and Jones | 260-395 **right** (skipped) | gates, fangio-2026 |
| Jones | left, about 320-460, outside of T1 | 345-565 outside (ok) | both |
| Brabham | right/inside of T1-T2, about 400-520 | ok | both |
| Hill | left/outside of T3, about 1060-1140 | ok | both |
| Ricciardo | right/inside between T3 and T4, about 1150-1230 | missing | both |
| Stewart | left/outside between T5 and T6, about 1550-1750 (map: on the run T5->T6) | 1600-1720 (ok) | both |
| Button | left/outside of T8 apex, about 2080-2140 | missing | both |
| Clark | left/outside of the T8->Lakeside sweep, about 2200-2350 | ok | both |
| Waite | left/inside of T9-T10, about 3300-3450 | 1860-2030 (at T6-T7) | both |
| Vettel | left/outside on the approach to T11, about 4000-4110 | missing | both |
| Webber | right/inside of T11, about 4150-4300 | 3300-3420 (at T9) | both |
| Lauda | left/outside of T12, about 4380-4480 | 3440-3560 (at T10) | both |
| Schumacher | left, between T13 and T14, about 4700-4770 | 3740-3860 (on a kink) | both |
| Prost | left/outside round T14, about 4760-4870 (curved) | ok | both |
| Senna | left, about 4900-5020, covered | ok position; covered=False | both |
| Footbridges | over the straight beside Fangio (about 5100), at T8/Clark (about 2200), at T11 (about 4150) | none | dossier text (maps do not draw them) |
| Albert Park Lake | inside the lap, the whole interior between T2-T8 and Lakeside Drive back to the straight | none | gates (interior) |

## Kit gaps

- `tree/eucalypt_l` (a river red gum / blue gum): Australian parkland.
  Albert Park, and later every Australian venue.
- People family (see the People cell).

## Rule gaps

- **Floodlights at a day race:** `DAY_RACE_NO_FLOODLIGHTS` only strips OSM
  masts in osm_layout.py. dress.rs then sees `has_lighting == false` and
  `style.floodlights == true` and lays 24 generated towers. Either the
  dossier must carry a "no floodlights" flag that dress reads, or the style
  needs it.
- **Park-street barrier style:** concrete blocks + debris fence close to
  the road (like STREET, but with grass ground and a tree belt). Albert Park
  needs it, and probably ChillyVilleneuve too.

## Surprising

- The centerline is still the **pre-2021 layout**: a right-left chicane at
  2520-2670 on Lakeside Drive, and the guide lists 16 corners. The dossier's
  modelling pitfall says "Do not model a chicane here". This is a track
  geometry issue upstream of dressing. Because of it, MANUAL_STANDS took the
  fast left-right at 3320/3450 for T11/T12, which is what misplaced
  Waite/Webber/Lauda/Schumacher.
## S/F dossier pass

In the tables:

- Removed: Fangio, Moss, Senna.
- `MANUAL_STANDS`: Senna (4880-4990 left covered); Fangio (5020-5205 left); Piastri (5225-85 left); Moss (235-330 left).

Left, or for later columns:

- The pit lane is the real S/F fault and is not in these tables: MANUAL_PIT_LANE has it 0-380 right, so lane, garages and podium sit between the line and T1. Both maps draw it leaving at T13/T14 and merging about 200 m before T1. Proposed (checked: every stand above stays 29+ m from its nodes, on the other side of the road): "AlbertParkour": dict(from_m=4760.0, to_m=140.0, side="right", gap_m=8.0), and its comment ("0-380, pit exit rejoins at Turn 1") rewritten with it. The support garages are at the entry end (opposite Senna), the F1 garages opposite Fangio/Piastri; one generated garage row cannot show the split.
- The screens are at 5100/5240 only to about 50 m (text, not a map). They stand on the ground behind the garages, not on the roof: no rule lifts a screen onto the pit building (rule gap, also Armadillos and Bahrain). A generated pit building taller than about 11 m hides them from the stands.
- Prost (4780-4870 outside) and Jones (345-565 outside) agree with both maps within 50 m and stay. The podium's TV backdrop is the export's.
- The footbridge beside Fangio (about 5100) is Landmark-column work.
- After re-applying: Moss must now be laid (it was skipped on the right), and none overlaps: Senna 4880-4990, Fangio 5020-5205, Piastri 5225-85, Moss 235-330, Jones from 345.
- The two pit-building screens are left out: on the ground behind the garages they take the football club and fitness centre buildings that stand there out of the dossier.

## Landmark dossier pass

In the tables:

- `MANUAL_CROSSINGS`: Fangio footbridge (5005), to the Main Stage / Fan Zone; no map draws it, set between Senna and Fangio.

Left, or for later columns:

- The venue's landmark is Albert Park Lake: R16.
- The T11 footbridge to Webber is in the dossier text but on no map: not placed.
- The pit-building screens: not placed (they stand on the clubhouses' outlines' reach before R19 was narrowed; re-read on the trace page now that R19 takes only the outline a landmark stands on).
- Lakeside Stadium: laid by R14 as four blocks on its side of the road (the rest of its outline reaches across).

## Stands dossier pass

In the tables:

- Waite, Webber, Lauda and Schumacher moved to their real corners (T9-T10, T11, T12, T13-T14; they stood 800-1400 m early); Ricciardo, Button and Vettel added (2026 grandstand map and the gates map).

Left, or for later columns:

- Temporary stands deeper than 6 m keep the regular bays: the kit's `scaffold_10m` (5 m deep, 4 m tall) is a low bleacher.
- The AmEx stand is on no map.
- The general-admission areas are flat lawns: no banks.

## Trees dossier pass

In the tables and rules:

- `AUSTRALIAN` style (gums among broadleaf). `MANUAL_WOODS`: the golf-course belt along Lakeside Drive and park belts between the stands (nine); they replace the generic belt, which also stood where the lake is.

Left:

- The lake itself (R16): OSM's relation 346429 is not read as water.
