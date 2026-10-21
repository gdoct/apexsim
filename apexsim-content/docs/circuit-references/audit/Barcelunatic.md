# Barcelunatic (Circuit de Barcelona-Catalunya, Montmeló) - dressing audit

Sources: dossier `circuit-references/Barcelunatic.md`; map
`barcelona-2026-spectator-map.png` (read as an image, the PNG of the
spectator PDF); the `ats-dress --explain` output; `Barcelunatic.layout.json`;
`osm_layout.py` MANUAL_STANDS "Barcelunatic" (Tribuna F only) and
CORNER_DISPLAY "Barcelunatic".

Station frame: the dossier's corners are not usable. They are three OSM
names ("Circuit de Barcelona-Catalunya", "Rallycross", "AZ"), all with
display None. So this is read off the curvature. S/F is at 0 and the lap
is 4642 m. T1 R 790-870, T2 L 900-970, T3 R 1050-1310, T4 R 1660-1890,
T5 L 2070-2180, T6 L about 2350, T7 L 2510-2590, T8 R 2610-2660,
T9 R 2840-2970, T10 L 3440-3530, T11 L 3590-3640, T12 R 3660-3850,
T13 R 3970-4040, **L-R chicane 4100-4150 / 4170-4210 (RACC, not used since
2023)**, T14 R 4260-4400.

## Cells

| Column | Cell | Reason |
|---|---|---|
| S/F | dossier | The pit lane (1037 m, right, running on through T1) and the generated garages are right. The Main Grandstand "Graderia principal" (OSM, 289 m, depth 24.6) is laid uncovered as `bay_10m_large`. The real stand is permanent and covered, in two tiers, with the Sky View lounge on top (dossier "Buildings"), so it needs covered=True. The control/scoreboard tower on the pit side near the pit exit, opposite J (dossier S/F), is missing; a `tower` landmark (`control_tower`) stands in. The podium (pit-entry half, unconfirmed) is generated on the middle garage, which is acceptable. |
| Landmark | dossier | The **Circuit Rooftop** building spans the track on the approach to La Caixa: map item 7, the orange box drawn across the road. OSM has it as "VIP Rooftop" (3361 R), and explain skips it with "reaches across the road". The kit has `bridge/span_building` (8 x 21 x 22 m), which only a `MANUAL_FURNITURE` bridge row can lay: no crossing kind maps to it. The two OSM footbridges at 1634 and 2316 are laid as truss_bridge. The heliport (map "H" inside T10) is minor. |
| Stands | dossier | 15 named stands are laid. Missing are **Tribuna I** (covered, outside the final corner) and **Tribuna V** (new on the 2026 map, between Principal and J). The covered flags are wrong: the dossier marks Principal, H, I, L and M as covered. Of those only L is roofed (depth 37.8), and H is laid as small `bay_10m`. |
| Trees | dossier | There are 5 OSM woods, all `leaf: mixed`, and only about 58 real trees (plus 3600 grass/wildflower clumps and 127 forest impostors). The circuit sits in Vallès farmland with Mediterranean pine (Aleppo/stone pine) belts on the banks round T3-T9 and behind the stadium. The woods want `needleleaved` leaf and a MANUAL_WOODS belt or two. Kit: an umbrella/stone pine would be truer than `conifer_m`. |
| Barriers | ok | Armco 1083, armco_4m_fence 555 (behind the stands), tecpro 1397, tyres 51. That fits a gravel-and-catch-fence permanent circuit. Not checked per cell. The 2026 tyre barrier on the right of the pit-entry road is not modelled. |
| Boards | dossier | No usable corners, so no corner signs are laid and only 10 marshal posts. The corners need names: T1 Elf, T3 Renault, T4 Repsol, T5 Seat, T9 Campsa, T10 La Caixa, T12 Europcar, T14 New Holland, with parody `display_name`s. 440 hoardings, 11 braking boards and 11 named car parks are ok. Also the rule gap: 19 generated `floodlight_tower`s at a day circuit. The map shows about 20 "Live screens" round the lap and 8 GA big screens (dossier GA row), and none are laid (`screen` landmarks). |
| People | kit | The Pelouse GA lawns (OSM "Pelouse" grass areas, the hill between T6 and T8), Pelouse Privilege, Pelouse Jove and fanzones behind Principal and at N. Wanted: crowds sitting on grass banks, standing crowd at the fence, fanzone crowds, marshals. |

## Positions from the maps

All are left/right in the driving direction. Stations come from the
map's straight at about 1.4 m/px, T14 exit (4400) to T1 (790).

| Structure | Should be | Dossier today | Map |
|---|---|---|---|
| Tribuna I | left, outside the final corner T14 onto the straight, about 4330-4420, covered | missing (Garden Club building at 4300 L) | spectator-map |
| Tribuna V | left, between Principal and J, about 260-350 | missing | spectator-map |
| Principal | left, about -40..265 (ok), covered | uncovered | spectator-map + dossier |
| Control/scoreboard tower | right, behind the pit lane opposite J, about 430-470 | missing | dossier S/F (not drawn on map) |
| Circuit Rooftop | spanning the road, about 3340-3400 (the run into T10) | OSM "VIP Rooftop" skipped | spectator-map item 7 |
| Live screens | about 20: along the straight both sides, at T1, A, L/M, N, the back straight, T10, G/B/C, H, I | none | spectator-map "Live Screens" icons |
| F1 Fanzone stage | outside of T9 / behind N, about 2850, about 120 m off | none | spectator-map (F1 Fanzone Stage icon) |
| Media centre | right, in the paddock block behind the pits, about 150-250 | unnamed buildings only | spectator-map |
| Tribuna T1 | left/outside of T1, about 830-870 | OSM 857 L, 85.6 m off (ok) | spectator-map |
| Tribuna T10 | right/outside of La Caixa, about 3450-3500 | OSM 3475 R (ok) | spectator-map |

## Kit gaps

- `tree/stone_pine_l` (umbrella pine): the Mediterranean belts (also
  Monzarella's park, and others).
- People family.

## Rule gaps

- **No crossing kind lays `bridge/span_building`**, so a dossier building
  that spans the road (Circuit Rooftop) is skipped as "reaches across the
  road". A `span` crossing kind, or a structure-to-span rule, would let OSM
  outlines like this become the kit span.
- **Floodlights at day venues** (as at AlbertParkour).

## Surprising

- The centerline takes the **RACC chicane** (L-R at 4100-4210). Real F1
  has bypassed it since 2023. The README's "what to check" flags this
  (final chicane = 2007-2022), and the dossier says the tarmac still exists
  but is not raced. T10 is also very tight (peak about 128 deg in 60 m),
  which may be the pre-2021 hairpin. Both are track-geometry issues
  upstream of dressing.
## S/F dossier pass

In the tables:

- `MANUAL_STANDS`: Tribuna V (305-390 left); Tribuna I (4335-4420 outside covered).
- `STAND_COVER`: Graderia principal covered.
- `MANUAL_LANDMARKS`: screen at 4517.0 right; screen at 287.0 right; screen at 475.0 right; screen at 628.0 right; screen at 705.0 right; tower at 465.0 right.

Left, or for later columns:

- Principal (OSM, front about -34..255 left) agrees with the map within 50 m; only its roof was wrong. J, K, E (OSM) and F (manual) stay.
- The control tower is from text only (to about 50 m); drop it if a photo shows it elsewhere. The screen at 628 is 29 m from the pit lane's last nodes (the lane converges on the road there), the closest to the pit zone of the set: re-check it in the dress run.
- Tribuna I is on the final corner rather than the straight proper; it is here because the map draws it facing the start of the straight. H, L and M (covered in the dossier, laid partly uncovered) are Stands-column work.
- The live screen at the T14 inside (map, about 4400 right, beside the pit entry road) is left out: it falls on the pit entry and the map position is too loose to clear it.
- The podium (pit-entry half, unconfirmed) stays on the middle garage.
- The Media Centre (right, about 150-250, behind the pits) is a block behind the generated pit complex, not placed.
- The RACC chicane in the centerline (4100-4210) is a geometry issue upstream of dressing; Tribuna I's span starts after it.

## Landmark dossier pass

In the tables:

- `MANUAL_CROSSINGS`: Circuit Rooftop (3361.2), a `building` crossing (`span_building`, R5) where OSM's "VIP Rooftop" reaches across the road.
- `MANUAL_LANDMARKS`: F1 Fanzone stage, 2940 left, 150 m off (2026 spectator map, affine fit, 9-58 m).

Left, or for later columns:

- The kit's span building is 8.2 m along the road against a real ~70 m building: a second module if the shots look thin.
- The heliport: no kit piece.

## Stands dossier pass

In the tables:

- Roofs: H, L and M covered, G and N open (the depth rule roofed them). Five banks on OSM's "Pelouse" general-admission areas (T6, T7, T8, La Caixa, Pelouse Privilege inside T1).

Left, or for later columns:

- A's partial roof.
- The RACC chicane in the centerline (follow-ups).

## Trees dossier pass

In the tables and rules:

- `MEDITERRANEAN` style (umbrella pines over broadleaf and scrub). `MANUAL_WOODS`: pine belts outside T3/T4, behind the T6-T8 Pelouse banks, outside T9 and inside the stadium loop.

Left:

- "Behind the stadium" is car parks and roads in the layout: no belt there.

## Boards dossier pass

- `MANUAL_CORNERS` (R10): 14 turns, the sponsor names as sound-alikes; the RACC chicane on the centerline gets no corner. T11's name is unconfirmed.
- Marshal posts every 150 m along the straights (the rule).
