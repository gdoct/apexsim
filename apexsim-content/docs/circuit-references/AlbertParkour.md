# Albert Park Circuit, Melbourne (`AlbertParkour`)

Modelled on: Albert Park, Melbourne, Victoria, Australia (temporary street/parkland circuit around Albert Park Lake, with permanent pit buildings). In game: 5.29 km, F1. Real (current layout): 5.278 km, 14 turns, clockwise.

## Which layout
- **Current: Grand Prix Circuit 2021 works, raced from 2022.** 5.278 km, 14 turns. Works done after the 2020 and 2021 races were cancelled (built for the planned Nov 2021 date, first raced April 2022): whole track resurfaced (first time since 1996); Turn 1 widened 2.5 m to the drivers' right; Turn 3 widened up to 4 m on the inside with camber change; Turn 6 inside moved 7.5 m (minimum speed reportedly rises from 149 to 219 km/h); the slow **Turn 9-10 (Clark) chicane removed**, making Lakeside Drive one flat-out run of about 1.3 km; the old Turn 13 (now Turn 11) realigned with Ross Gregory Drive and a short extension of Lakeside Drive and made 3 m wider on the inside; the penultimate corner (old T15, now T13) widened 3.5 m; camber changed at new T3, T11, T13; pit lane widened about 2 m (stated aim: raise the pit speed limit from 60 to 80 km/h).
- **Renumbering:** old T1-T8 keep their numbers; old T9/T10 are gone; old T11/T12 (fast left-right) become T9/T10; old T13 -> T11, T14 -> T12, T15 -> T13, T16 -> T14.
- **1996-2020:** 5.303 km (5.302 km before the FIA's 2000 re-measure), 16 turns, same direction. A pit-lane chicane existed until 2004. A second (support/Supercars) pit lane with temporary garages was added in 2011.
- **1953-1958 original:** 5.027 km, 9 turns, run **anticlockwise** on the park roads. Not relevant for modelling except that the 1996 track re-used many of those roads in the opposite direction.
- **In-game length 5.29 km** sits exactly between the current 5.278 and the old 5.303 km, so the length alone does not tell which layout was modelled. The test is the back straight: if there is a slow right-left chicane on Lakeside Drive halfway between T8 and the fast left-right (T9/T10), the model is the pre-2022 layout. The category note says 2022 reconfiguration: there must be no chicane there and 14 numbered turns.
- **Supercars/support layout:** same track, but support series use the support pit lane/garages at the south (start) end of the long pit lane (racingcircuits.info shows a separate "Supercars Circuit" map for 2021-to-date).

## Maps
| Map | Kind | Year | Local file | URL |
|---|---|---|---|---|
| F1 official detailed track map (corner numbers, Straight/Overtake mode zones) | circuit-map | 2026 | `maps/AlbertParkour/f1-2026-melbourne-detailed.webp` | https://media.formula1.com/image/upload/c_lfill,w_3392/q_auto/v1740000001/fom-website/2026/Circuit%20maps/2026trackmelbournedetailed.webp |
| Albert Park gates and entrances map (all grandstands, gates, roads) - fan site oversteer48 | grandstand-map | 2024 | `maps/AlbertParkour/oversteer48-gates-entrances-map.jpg` | https://oversteer48.com/wp-content/uploads/2024/01/Albert-Park-Gates-and-entrances-map-F1.jpg |
| Fangio grandstand location map (2026 grandstand plan incl. Piastri stand) - oversteer48 | grandstand-map | 2026 | `maps/AlbertParkour/oversteer48-fangio-grandstand-map-2026.jpg` | https://oversteer48.com/wp-content/uploads/2023/12/Fangio-Grandstand-map-Australian-Grand-Prix-2026.jpg |
| Official AGP program track overview | circuit-map | 2022+ (unconfirmed exact year) | `maps/AlbertParkour/agp-program-track-overview.png` | https://tlc-endpoint-h2gjb2fjapgef8ar.z03.azurefd.net/media/s3lhpxs2/f89a110f-9fdf-4f3b-be3f-92c59f185efb.png |
| racingcircuits.info Grand Prix circuit 2021-to-date | circuit-map | 2021 | `maps/AlbertParkour/racingcircuits-melbourne-2021.png` | https://www.racingcircuits.info/assets/images/maps/Australasia/Australia/Albert%20Park/Melbourne-2021.png |

No official promoter wayfinding PDF was found: grandprix.com.au/event/map is an interactive (Mappedin) map that could not be fetched; a Wikimedia Commons SVG of the 2021+ layout could not be confirmed (Commons was unreachable).

## Corners
Corner names are historic and sources disagree on them (e.g. one guide gives T1 "Brabham"/T2 "Jones", another T1 "Jones"/T2 "Brabham"); the official F1 and AGP material uses numbers only. Directions for T12/T13 also differ between guides (flagged).

| T | Name | Dir | Notes |
|---|---|---|---|
| 1 | Jones (or Brabham) | R | End of ~0.8 km start/finish straight; heavy braking, widened 2.5 m in 2021. Jones grandstand outside, Brabham grandstand inside (infield). Common lap-1 incident point. |
| 2 | Brabham (or Jones) | L | Fast left straight out of T1. |
| 3 | Sports Centre (old guides: Whiteford) | R | Slow, heavy braking; widened up to 4 m on the inside, camber changed. Hill grandstand outside (outfield), Ricciardo grandstand infield between T3 and T4. Named for the adjacent Melbourne Sports and Aquatic Centre (unconfirmed exact adjacency). |
| 4 | Hellas | L | Short chute from T3; tricky left. |
| 5 | Whiteford | R | Fast, flat right. Stewart grandstand at the exit (Gate 3, Albert Road side). |
| 6 | Marina | R | Was a tight 90-degree right; inside moved 7.5 m in 2021, now much faster (~219 km/h min). |
| 7 | Lauda | L (unconfirmed) | Flat out; runs past Albert Park Golf Course. |
| 8 | - | R | Fast right onto Lakeside Drive. Button grandstand at T8 (outfield), Clark just past T8 (outfield, by the main pedestrian bridge over the lake), near Gate 5. |
| - | (old T9/T10 Clark chicane) | - | Removed in 2021; now flat-out ~1.3 km along Lakeside Drive. Do not model a chicane here. |
| 9 | Waite (old T11) | L | Very fast left (~330 km/h, lift), start of the fast left-right flick. Waite grandstand (Gate 8, Queens Road side). |
| 10 | (old T12) | R | Fast right flick out of T9 into the braking zone for T11. |
| 11 | Ascari (old T13) | R | Heavy braking, ~90 degrees; prime overtaking spot; realigned with Ross Gregory Drive and widened 3 m in 2021. Vettel grandstand across (outside, Gate 9), Webber grandstand on the inside (reached by footbridge; trees to its left, a hospitality building to its right). |
| 12 | Stewart (old T14) | L (sources disagree: one says R) | Medium speed, blind apex. Lauda grandstand on driver's left, view back to the city. |
| 13 | (old T15; names "Senna"/"Prost" used inconsistently) | R (sources disagree: one says sharp L) | Slow, widened 3.5 m in 2021, camber changed; overtaking possible. Schumacher grandstand between T13 and T14, opposite pit entry. |
| 14 | (old T16; "Prost"/"Senna") | R | Final corner onto the pit straight. Pit entry on the driver's right here. Prost grandstand looks down on the T14 apex. Senna grandstand at its exit. |

## Start/finish, pit lane, paddock
- **Pit straight** runs roughly north-west/north from T14 (south-west of the lake, Gate 10/Fitzroy Street end) to T1 (Gate 1/Canterbury Road side). Pole position is on the left of the grid.
- **Pit lane on the driver's right (infield, lake side)**; grandstands (Senna, Fangio, Piastri, Moss) line the outside (driver's left).
- **Pit entry**: on the right at the final corner (T14); visible from Senna, Prost and Schumacher grandstands.
- **Pit exit**: at the north end of the pit straight, rejoining on the right before T1 (old guides: about three-quarters of the way down the straight, so exiting cars merge into the T1 braking zone). No exit-line crossing documented.
- **Double-length pit lane**: the garages at the start (south end) of the lane, opposite the Senna grandstand, are for support series (Supercars etc.); the F1 garages are further along, opposite Fangio/Piastri. Separate support pit lane added 2011 per racingcircuits.info.
- **Pit Building 1** (northern end of the start/finish straight, near pit exit) houses **Race Control** and the **FIA Media Centre** - a two-storey building; press conferences on the first floor. It sits straight ahead at the end of **Melbourne Walk**, the walkway that comes from the lake (pontoon bridge) to the paddock.
- **Pit buildings** are permanent (built 1996); everything else (grandstands, fences, overpasses) is temporary, built ~2 months before the race and removed within ~6 weeks.
- **Podium**: on the pit side, opposite Fangio sections B/C and Piastri sections C-E, i.e. near the start/finish line; a TV screen forms its backdrop. Two large screens sit on top of the pit building.
- **Paddock**: behind the pit buildings, between them and the lake (Melbourne Walk/pontoon side). Exact Paddock Club location unconfirmed.
- **Start line vs finish line**: not documented as separate; treat as one line opposite Fangio/Piastri.

## Buildings and structures
| Structure | Where | Notes |
|---|---|---|
| Moss grandstand | Outside of main straight just before T1 (driver's left) | Near Gate 1 (Canterbury Road); wheelchair seating, screen |
| Piastri grandstand (new 2026) | Outside of start/finish straight, opposite pit garages/podium; between Moss (towards T1) and Fangio | Took the end section of Fangio; section A nearest last corner, J nearest T1 |
| Fangio grandstand | Outside of main straight, opposite the pit buildings | Gate 1; a pedestrian bridge over the track next to it leads to the Main Stage/Fan Zone (infield) |
| Senna grandstand | Outside of pit straight at the exit of T14; between Fangio and Prost | Faces the pit entry and support garages; covered (premium) |
| Prost grandstand | Looking down on the T14 apex, opposite pit entry | Gate 10 |
| Schumacher grandstand | Between T13 and T14, view of T13, opposite pit entry | Gate 10; near "Legends Lane" |
| Jones grandstand | Outside of T1 | Gate 1 |
| Brabham grandstand | Infield (inside) of T1-T2 | Near Melbourne Walk, M-Lane |
| Hill grandstand | Outside of T3 | Gate 3 (Albert Road); no roof |
| Ricciardo grandstand | Infield between T3 and T4 | Gate 2; part covered |
| Stewart grandstand | Exit of T5 | Gate 3 |
| Button grandstand | Outside of T8 | Gate 5; replaced "The Balconies" in 2023 |
| Clark grandstand | Outside, just past T8, overlooks Lakeside Drive | Gate 5; next to the main pedestrian footbridge over the lake |
| Waite grandstand | Lakeside Drive section (T9/T10 area) - exact side unconfirmed | Gate 8 (Queens Road) |
| Vettel grandstand | Outside of T11 | Gate 9; footbridge needed to reach the infield |
| Webber grandstand | Inside of T11 | Reached via a footbridge over the track; hospitality building to its right |
| Lauda grandstand | T12, driver's left (outfield) | Gate 10; city skyline view |
| AmEx grandstand | Location unconfirmed | Listed on oversteer48 |
| Pit Building 1 | North end of pit straight, inside | Race Control, FIA Media Centre (two storeys) |
| Support pit garages | South (entry) end of pit lane, inside | Supercars/support use |
| Melbourne Walk | From the lake pontoon to the paddock/pit building | Driver arrival walkway |
| Pontoon bridge(s) over Albert Park Lake | Across the lake from the golf course side to Melbourne Walk | Temporary |
| Pedestrian overpasses over the track | "Many dotted around the track": confirmed next to Fangio (main straight), near T8/Clark, at T11 (Webber/Vettel) | Temporary; exact count unconfirmed |
| Albert Park Lake | Inside the circuit; the track runs round it | Main water feature; city skyline behind to the north |
| Albert Park Golf Course | East/north-east side, by T7-T8 | International Media car park during the event (Gate 5, Kings Way) |
| Lakeside Stadium | West side; F1 says the track detours through the Lakeside Stadium car park (exact corner unconfirmed, likely T1-T2 area) | |
| Melbourne Sports and Aquatic Centre | North-west, near T3 ("Sports Centre") - unconfirmed exact position | |
| Main Stage / Fan Zone | Infield, reached by bridge from Fangio | |
| Village Green | South end of circuit | National media car park (Gate 10 via Fitzroy Street) |
| Public roads | Track uses Aughtie Drive and Lakeside Drive; Ross Gregory Drive at T11; outside roads: Canterbury Road (W/SW, Gates 1-2), Albert Road (N, Gates 3/5), Kings Way (NE), Queens Road (E, Gates 8-10), Fitzroy Street (S) | |

## Terrain and elevation
Essentially flat: total elevation change about 3 m (f1-fansite). Bumps and low grip are the features, not gradients.

## Key coordinates
- Circuit (Wikipedia infobox): -37.84972, 144.96833 - https://www.openstreetmap.org/#map=16/-37.8497/144.9683
- Start line, pit entry, pit exit: unconfirmed (OSM/Overpass could not be queried). Relative: start line on the west side of the lake on the pit straight; pit entry at T14 (south end); pit exit at the north end before T1.

## Modelling pitfalls
- **The Lakeside Drive chicane (old T9/T10) is gone since 2021.** Old maps and games show 16 turns; current has 14 with renumbered corners (old 11-16 -> new 9-14).
- T6 is much faster/wider than on old maps; T11 (old T13) moved slightly (realigned with Ross Gregory Drive).
- In-game 5.29 km fits neither layout exactly (5.278 vs 5.303): check which layout the centreline actually follows.
- Temporary vs permanent: only the pit buildings (Pit Building 1 + support garages) are permanent. Grandstands, footbridges, pontoons and fan-zone stages are temporary; the rest of the year the track is ordinary park roads with traffic.
- Long double pit lane: support garages first, F1 garages further along; pit entry at the last corner on the right, exit near T1 on the right.
- Corner names in circulation conflict; use numbers.

## Sources
1. [Albert Park Circuit - Wikipedia](https://en.wikipedia.org/wiki/Albert_Park_Circuit)
2. [CIRCUIT GUIDE: Albert Park 2026 - Formula1.com](https://www.formula1.com/en/latest/article/circuit-guide-2026-australian-grand-prix-albert-park.19zPlhKhMbTaVNFIPKAAMa)
3. [FIA 2026 Australian GP press kit (PDF)](https://www.fia.com/sites/default/files/gp26_press_kit.pdf)
4. [FIA 2025 Australian GP media kit (PDF)](https://fia.com/sites/default/files/media_kit_-_2025_australian_grand_prix_.pdf)
5. [Albert Park - racingcircuits.info](https://www.racingcircuits.info/australasia/australia/albert-park,-melbourne.html)
6. [Albert Park F1 layout changes explained - motorsport.com](https://au.motorsport.com/f1/news/albert-park-layout-changes-explained/5430789/)
7. [AGP official program: Track overview](https://program.grandprix.com.au/issue-0-formula-1/track-guide/)
8. [GP23 Grandstand Information (Ticketmaster PDF)](https://media.ticketmaster.com/en-au/img/static/pdf/GP23_GrandstandInformation.pdf)
9. [Grandstand guide - travel.grandprix.com.au](https://travel.grandprix.com.au/blog/grandstand-guide)
10. [Albert Park gates and entrances - oversteer48](https://oversteer48.com/albert-park-gates-entrances-f1/)
11. [Piastri grandstand - oversteer48](https://oversteer48.com/piastri-grandstand-australian-grand-prix/)
12. [Fangio grandstand - oversteer48](https://oversteer48.com/?p=28670)
13. [Senna grandstand - oversteer48](https://oversteer48.com/senna-grandstand/)
14. [Webber grandstand - oversteer48](https://oversteer48.com/webber-grandstand/)
15. [F1 Australian GP: a detailed look at Albert Park - GPFans](https://www.gpfans.com/en/f1-news/1015879/f1-australian-grand-prix-albert-park-circuit/)
16. [Albert Park F1 circuit guide - GPFans](https://www.gpfans.com/en/f1-news/1077012/albert-park-f1-circuit)
17. [F1 2019 Australian GP track guide - RealSport101](https://realsport101.com/article/f1-2019-australian-grand-prix-track-guide)
18. [Albert Park Grand Prix Circuit - F1 Wiki (Fandom)](https://f1.fandom.com/wiki/Albert_Park_Grand_Prix_Circuit)
19. [Albert Park Circuit - f1-fansite](https://www.f1-fansite.com/f1-circuits/albert-park-circuit/)
