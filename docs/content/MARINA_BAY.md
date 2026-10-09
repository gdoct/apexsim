# Mandarina Bay (Marina Bay Street Circuit, Singapore)

Research pass, 2026-10-09; the circuit itself was built the same day
(`content/tracks/default/MarinaBay/`, CLAUDE.md "Mandarina Bay"). This file is
the survey that came before it, plus the OSM-derived seed for the dossier
(`MARINA_BAY.dossier-seed.json`, beside this file). The sections below are
the research as written: the lap is not routed from waypoints but from OSM
relation 421263, which models the whole route.

## 1. Identity

| | |
|---|---|
| Stem | `MarinaBay` |
| `name` (YAML, real) | Marina Bay Street Circuit |
| `display_name` (what the player sees) | **Mandarina Bay** |
| Description | "Modelled on the street circuit around Marina Bay, Singapore." |
| Category | Formula (F1) |
| Layout to build | current 19-turn layout (2023 on), counter-clockwise |
| Length | about 4.93 km (Wikipedia gives 4.927 km for the current layout and 4.940 km in the 2023-24 lap-record table; use the routed length, not either figure) |
| `track_id` | generate one UUID when the YAML is made and never change it |

Per the no-trademarks rule (CLAUDE.md), real corner names stay in `name` and
the player sees `display_name`. Names that are places (Sheares, Memorial,
Fullerton) can stay; "Singapore Sling" (Turn 10) is a cocktail trademark and
needs a sound-alike. Proposed, none authored: T1 `Sheares` as is, T7
`Memorial` as is, T10 `Singa Swing`, T13 `Fullerton` as is.

Layout history, for picking the right traces: T10 chicane removed 2013;
T11-13 reprofiled 2015; T16-19 over the floating platform replaced by the long
Raffles Avenue straight in 2023. OSM's raceway fragments below are from the
pit-straight area, which did not change.

## 2. Data

**Source.** Geofabrik `asia/malaysia-singapore-brunei-latest.osm.pbf`
(240 MB), OSM data timestamp **2026-10-08T20:03:26Z**, loaded into
`wiktorn/overpass-api` (Docker Desktop on the dev machine, named volume
`overpass-sg-db`, port 12345).

**Run it again**

```powershell
docker run -d --name overpass-sg -p 12345:80 -v overpass-sg-db:/db `
  -e OVERPASS_MODE=init -e OVERPASS_META=no `
  -e OVERPASS_PLANET_URL=https://download.geofabrik.de/asia/malaysia-singapore-brunei-latest.osm.pbf `
  -e OVERPASS_PLANET_PREPROCESS="mv /db/planet.osm.bz2 /db/planet.osm.pbf && osmium cat -o /db/planet.osm.bz2 /db/planet.osm.pbf && rm /db/planet.osm.pbf" `
  wiktorn/overpass-api
docker start overpass-sg      # init ends with "initialization complete. Exiting."; start it again to serve
curl.exe -s http://127.0.0.1:12345/api/status
```

Import took about 20 minutes here (pbf to bz2 conversion, then the three passes, then area generation).
Traps hit: from bash use `\` not backticks; keep the database in a Docker
volume, not a Windows folder; **use `127.0.0.1`, not `localhost`** from Chrome
(`localhost` hangs, probably IPv6).

**Extract.** `.cache/osm/MarinaBay.query.overpassql` wrote
`.cache/osm/MarinaBay.0.json` (gitignored like every extract): every node,
way and relation (boundaries excluded) touching
`S 1.2830, W 103.8460, N 1.2990, E 103.8720`, plus members. 31 MB, 107 885
nodes, 30 012 ways, 988 relations, same JSON shape `osm_layout.py` reads.

```bash
curl.exe -s --data-urlencode "data@E:\apexsim\.cache\osm\MarinaBay.query.overpassql" http://127.0.0.1:12345/api/interpreter -o E:\apexsim\.cache\osm\MarinaBay.0.json
```

Draft registry entry (not added): `"MarinaBay": [(103.846, 1.283, 103.872, 1.299)]`.
The box stops at 1.283 N, so **Gardens by the Bay (Supertree Grove) is outside
it**; extend south to about 1.278 if it is wanted as backdrop.

## 3. What OSM gives, and what it does not

**Raceway ways exist, for about 28% of the lap.** Five
`highway=raceway` ways named "Marina Bay Street Circuit", the pit lane and the
pit building:

| OSM | What | Length |
|---|---|---|
| w686335807 | main pit-straight way, south to north from (1.28870, 103.86284) to (1.29452, 103.86305); it bows about 100 m east in the middle (to lon 103.86444), so it is a long curve, not a straight | 865 m |
| w686335806 | approach into the pit straight | 101 m |
| w303311924 | approach before it, from (1.28955, 103.86108) | 150 m |
| w465064165 | the turn at the north end, to (1.29454, 103.86239) | 107 m |
| w767388313 | detached fragment on Esplanade Drive, (1.28926, 103.85427) to (1.29040, 103.85468) | 135 m |
| w100484287 | `raceway=pitlane`, 27 nodes, same end points as w686335807 | 826 m |

Four of the five chain end to end (1 223 m, counter-clockwise, which agrees
with the real lap). Total tagged: 1 358 m of a 4.93 km lap. **The other
3.6 km is ordinary public road and has to be routed.**

**The GPS database has no Singapore.** TUMFTM/racetrack-database (the source
of every other centerline) lists 25 tracks, none of them this one. So the
centerline is built the Nordschleife way: `scripts/osm_centerline.py` with a
new `LAPS["MarinaBay"]` waypoint list, but routed over the public street graph
for most of the lap, not over raceway ways.

Streets in the extract that the lap uses (Wikipedia's turn-by-turn, matched to
OSM names; whole-extract totals, so they overstate what the lap drives):

| Street | In extract | Lanes | Notes |
|---|---|---|---|
| Raffles Boulevard | 838 m, 29 ways | 3-6 | primary, one way; 3 ways on a negative layer or tunnel (underpass) |
| Republic Boulevard | 2 010 m | 1-3 | primary and `trunk_link`, one way |
| Nicoll Highway | 6 400 m | 3-5 | trunk, 8 bridge ways |
| Stamford Road | 1 036 m | 3-5 | primary |
| Saint Andrew's Road | 432 m | 2-4 | note the apostrophe: OSM says "Saint Andrew's Road" |
| Connaught Drive | 706 m | 1-2 | residential |
| Fullerton Road | 875 m | 2-4 | trunk plus service |
| Esplanade Drive | 1 127 m | 4-5 | trunk, 4 bridge ways (Esplanade Bridge) |
| Raffles Avenue | 1 079 m | 2-4 | primary, the 2023 long straight |
| Temasek Boulevard / Avenue | 1 023 m / 1 506 m | 1-3 | 2 ways underground on the Boulevard |

All asphalt, all one way, 50-70 km/h limits. The real circuit is wider and
closed to traffic, so road width has to come from lanes (about 3.3 m a lane)
with a manual widening table, as Norisring does.

**Pit lane.** OSM's lane is 826 m, 10-14 m off the main way (nearest-segment test),
on the **left** of the direction of travel (the inside of the counter-clockwise
loop). The pit building is at (1.29099, 103.86397), 8 053 m^2, 15 m tall,
`start_date=2008`. That gives the dossier a real `pit_lane` with no
`MANUAL_PIT_LANE`; check the side against satellite imagery as the task-card
rules require.

**No grandstands are tagged.** The race stands are temporary. Every stand has
to come from `MANUAL_STANDS` off the published seating map, with a comment
naming the source (definition of done, ADDITIONAL_TRACKS.md section 1,
items 3 and 11). Names are not sourced here.

**Elevation.** Marina Bay is reclaimed land, close to flat. Copernicus GLO-30
is a *surface* model and in a dense city it reads rooftops, the same trap the
Nordschleife note describes for canopy. Do not derive `z` from the DEM: start
flat and put real height only on the bridges and ramps (Anderson, Esplanade,
the Sheares viaduct end, the Raffles Boulevard underpass). `dem_fetch.py` and
`dem_elevation.py` are not worth running here. Unverified until the first
bake.

**Street lamps are not tagged** (zero `highway=street_lamp` in the box), so
lighting has to be laid by rule along both edges, not from OSM.

## 4. Iconic buildings to build as props

Numbers are from the extract (OSM id, footprint, height as tagged), distance to
the circuit corridor measured to the nearest road centreline in the lap's
streets. Asset keys avoid brand and place trademarks (the kit's
no-trademarks rule); the real name is in the first column for reference only.
Heights are OSM's `height` or 3.4 m x `building:levels`, and some disagree
with themselves (One Raffles Place Tower 1 appears as 215 m and 283 m in two
parts); treat them as relative.

### Status (2026-10-09): Tier A, B and C built (props complete)

All Tier A and Tier B props below now exist as GLBs under `content/props/`,
are in `track-editor/core/src/props.rs` (`KIT`) and in `PROPS.md` section 11.
Builders: `scripts/content/props/build_marina_bay_{a1,a2,a3,b1,c1}.py` (+ `marina_common.py`).

- Built: `landmark_twin_domes`, `landmark_big_wheel_xl` (+ `rotor` node), `building_wheel_terminal`,
  `building_pit_street`, `bridge_arch_steel`, `bridge_deck_wide`, `viaduct_deck`, `bridge_double_helix`,
  `building_colonnade_hotel`, `building_domed_court`, `building_colonnade_civic`,
  `building_clock_tower_hall`, `building_five_towers`, `fountain_basin`, `building_club_pavilion`,
  `building_gothic_church`, `building_deco_theatre`, `monument_four_columns`,
  `landmark_three_towers_skypark`, `landmark_lotus_museum`.
- Not built on purpose: `fountain_sea_lion` (the Merlion is a trademarked mascot; an original
  look-alike would read as the same thing, so the spot stays empty or gets a plain fountain).
- Tier C (section 5 gaps), built in `build_marina_bay_c1.py`: `quay_rail_4m`, `catch_fence_6m`,
  `traffic_signal_pole`, `road_sign_post`, `sign_gantry`, `linkbridge_covered`, `bus_shelter`,
  `station_entrance`, `carpark_entrance`, `raintree_l`, `lamp_arm_twin`, plus the three monuments
  `monument_obelisk`, `monument_pagoda`, `monument_statue_plinth` (all original designs, no likenesses).
  Fountains and the other viaduct/arch decks were already covered by Tier A.
- Still not props: the "water as ground" check (does `ats-export` draw `areas` water, and is the river
  crossing a bridge rather than ground) belongs to the track pipeline.
- Done 2026-10-09 (compiled, `ApexSim.Props.NightGlow` / `.Rotor` pass; not seen in game, the
  big wheel not re-imported yet): night-pass wiring for the `mb_*` window-glow slots and
  `mb_lit_panel` / `signal_*` (docs/content/PROPS.md, "Night pass additions"), and the
  `rotor` child of `landmark_big_wheel_xl` (0.033 rpm).
- Done 2026-10-09: the road-on-bridge rule (crossing kinds `deck_arch` / `deck_wide` carry
  `from_m`..`to_m`; `ats-dress` tiles the deck along the span, the `.ats` records a `bridges`
  span, and the bake leaves bands and curbs out of it) and "water as ground" (the dossier's
  `water` layer becomes `.ats` `water`; the terrain sinks under it, the bake draws a glossy
  `scenery` surface at `water_level_m`, nothing dressed stands in it). See CLAUDE.md,
  "Mandarina Bay".

### Tier A: on camera from the road (P1)

| Landmark | Asset key | OSM / size | From road | What makes it unmistakable |
|---|---|---|---|---|
| Esplanade, Theatres on the Bay | `landmark_twin_domes` | w256163012 5 874 m^2, w256163011 5 152 m^2, both 35 m, `roof=dome`; wrapper w97582570 10 436 m^2 | 56-92 m | two spiked, louvred domes; the kit has no dome of any kind |
| Singapore Flyer | `landmark_big_wheel_xl` + `building_wheel_terminal` | wheel w230082125, terminal r7227092/93/94 (21/14/7 m, 9 256 m^2); tagged parts up to 165 m | 61 m | the kit's `ferris_wheel` is 60 m across; this is about 150 m with capsules, plus a stepped terminal |
| F1 Pit Building | `building_pit_street` | w49890775 8 053 m^2, 15 m | 72 m | roof paddock and team garages on one block; the generated pit complex cannot make this |
| Anderson Bridge | `bridge_arch_steel` | r9574530 bridge, 301 m^2 | 32 m | Turn 12 uses its far side; a crossing, so `crossings`, not a building |
| Esplanade Bridge / Jubilee Bridge | `bridge_deck_wide` | 11 983 m^2 / 1 426-1 499 m^2 | 35-41 m | the lap crosses it (third DRS zone) |
| Benjamin Sheares Bridge | `viaduct_deck` | 74 404 m^2 | 65 m | elevated expressway above the pit straight's north end (T1 is named for it) |
| Fullerton Hotel | `building_colonnade_hotel` | w46595395 6 743 m^2, 25 m | 76 m | neoclassical block at the T13 hairpin; Fullerton Waterboat House 584 m^2 and One Fullerton 4 784 m^2 beside it |
| Swissôtel The Stamford | stand-in `skyline_needle`-class tower | w172364975 1 973 m^2, 226 m | 49 m | tallest thing near the road; Raffles City mall (22 793 m^2, 30 m) and Raffles City Tower (158 m) at its foot |
| Millenia Tower + Centennial Tower | `skyline_*` stand-ins, one bespoke | w48035153 1 726 m^2, 223 m; Centennial 158 m, 2 427 m^2 | 46-59 m | sit at the pit-straight end; Conrad hotel 4 164 m^2, 24 m |
| Suntec City | `building_five_towers` + `fountain_basin` | towers 1 782-3 485 m^2, convention centre 26 072 m^2, Fountain of Wealth 1 770 m^2 | 19-91 m | five towers around a large ring fountain |
| Marina Square / Mandarin Oriental / Parkroyal | stand-ins from `skyline_podium` | 53 159 m^2 mall, 5 276 m^2 hotel (curved), 9 842 m^2 | 48-108 m | long podium walls along Raffles Avenue |
| Padang with the cricket and recreation clubs | `area_padang` + `building_club_pavilion` | pitch 35 756 m^2, clubs 1 673 and 2 238 m^2 | 31-54 m | big open green inside T9-T10, the one place with open sky |
| Old Supreme Court / Old City Hall | `building_domed_court`, `building_colonnade_civic` | 5 111 m^2 25 m / 8 084 m^2 25 m | 40-66 m | white colonnaded row on the Padang edge |
| Victoria Theatre and Concert Hall | `building_clock_tower_hall` | 3 798 m^2, 15 m | 54 m | clock tower; The Arts House (1 866 m^2) next to it |
| Civilian War Memorial | `monument_four_columns` | 67 m tagged, 43 m^2 | 90 m | T7 is named after it |
| Merlion Park | `fountain_sea_lion` | park 5 369 m^2 | 57 m | **legal check first**: the Merlion is a Singapore Tourism Board mark. Build an original sea-lion fountain, or skip |
| Saint Andrew's Cathedral, Capitol Building | `building_gothic_church`, `building_deco_theatre` | 1 885 m^2 / 2 101 m^2, 34 m | 96 / 38 m | lower priority (P2) |

### Tier B: skyline across the water (P2, mostly existing kit)

The ten `skyline_*` buildings already cover the 112 building parts over 100 m in the
extract (66 over 150 m, 18 over 200 m). Only the following need their own
mesh because their silhouette is on every postcard:

| Landmark | Asset key | OSM / size | From road |
|---|---|---|---|
| Marina Bay Sands | `landmark_three_towers_skypark` | Tower 2 w172307472 3 087 m^2 and Tower 3 w172307471 1 582 m^2, both 193 m; SkyPark w116800998 207 m, 12 540 m^2; Infinity Pool w251802411; Shoppes r2298319 28 005 m^2, 15 m | about 460 m+ |
| ArtScience Museum | `landmark_lotus_museum` | r14314416 | about 340 m (relation centroid) |
| Helix Bridge | `bridge_double_helix` | r18007977 9 474 m^2 | 143 m |
| Raffles Place cluster | `skyline_*` | many unnamed 190-280 m parts around (1.2855, 103.8502), One Raffles Place 215/283 m, Singapore Land Tower 190 m | 350-400 m |

## 5. Other missing props

Counts are features within 40-50 m of the lap's streets, from the extract.

| Gap | Evidence | Proposed asset |
|---|---|---|
| Waterfront balustrade | quays and promenades along Marina Bay, Esplanade, Fullerton; OSM has `man_made=pier` and a 6 473 m^2 floating barrier | `barrier/quay_rail_4m` |
| Tall catch fencing | street circuits need 6-8 m fencing; kit's `mesh_4m` is 2.5 m | `fence/catch_fence_6m` |
| Traffic signals | 24 near the lap (55 in the box), 94 crossings | `sign/traffic_signal_pole` |
| Road signs and expressway gantries | Nicoll Highway, Sheares, ECP | `sign/road_sign_post`, `bridge/sign_gantry` |
| Bus shelters | 19 | `misc/bus_shelter` |
| Underground station entrances | 16 `subway_entrance` | `misc/station_entrance` |
| Car park ramp entrances | 13 | `misc/carpark_entrance` |
| Covered link bridges / footbridges | 42 bridge ways within 50 m of the lap (91 in the box); kit's `truss_bridge` is open | `bridge/linkbridge_covered` |
| Viaduct and arch bridge decks | see Tier A | `bridge/viaduct_deck`, `bridge/bridge_arch_steel`, `bridge/bridge_deck_wide` |
| Rain trees and angsana | 160 trees near the lap | `tree/raintree_l` (umbrella crown). Kit has `palm_ornamental` that "no track plants yet" and generic broadleaf |
| Fountains | 6 `amenity=fountain` | `misc/fountain_basin` |
| Monuments, plinths, memorials | 8 artwork plus Raffles statue, Lim Bo Seng pagoda memorial, Dalhousie Obelisk | `misc/monument_*` (the kit's only statue is the Spielberg bull) |
| Lighting | no lamps in OSM; the kit's ring only fires for circuits without masts | street lamp pair with arm (`light/lamp_arm_twin`) or a rule that lays `lamp_post` both sides |
| Water as ground | Marina Reservoir, Singapore River, Stamford Canal in `natural=water` / `waterway` | not a prop; check that `ats-export` draws `areas` water as a surface, and that the course crossing the river is a bridge, not ground |

Reuse as is: the 10 `skyline_*` towers, `palm_ornamental`, `hedge_4m`,
`concrete_4m` / `tecpro_2m` / `tires_*` barriers, `lamp_post`,
`floodlight_tower`, `hoarding_*`, `scaffold_10m` / `bay_10m*` stands (the
stands are scaffold in reality too), `timing_gantry`, `start_gantry`,
`hospitality_3f`, `media_centre`, marshal posts and corner signs.

## 6. Dossier

`MARINA_BAY.dossier-seed.json` holds the raw facts, not the pipeline's
`layout.json`: raceway ways with points, the pit building, 79 features within
120 m of the lap's streets, the across-the-bay set, and the 30 tallest
buildings. `osm_layout.py` cannot produce the real dossier yet: it fits the
extract onto a centerline by FFT plus ICP and refuses to write without one
(`fit.centerline_covered >= 0.9`, `fit.rmse_m <= 2.5`), and this track has no
centerline.

Mapping to the dossier's keys once the centerline exists:

| Key | Source | Manual? |
|---|---|---|
| `pit_lane` | w100484287, left of travel | no |
| `stands` | none in OSM | yes, `MANUAL_STANDS` off the seating map |
| `structures` | the Tier A and B buildings | partly; OSM footprints are good |
| `crossings` | Anderson Bridge, Esplanade Bridge, Jubilee, Sheares viaduct, Raffles Boulevard underpass | check each against the route |
| `landmarks` | `big_wheel` at the Flyer, `tower` for the tallest | yes: `dress.rs` knows only `big_wheel`, `screen`, `stage`, `camera_tower`, `tower`, `floodlight`, `blimp`, `balloon`, `helicopter` |
| `woods` | none | street circuit: leave empty. Do not invent trees; place the 160 mapped trees as rows |
| `areas` | Padang, War Memorial Park, Esplanade Park, Merlion Park | no |
| `barriers` | 3 jersey barriers, 2 fences, 2 retaining walls | no, groomer decides the rest |

## 7. Next steps, in order

1. Waypoints for `LAPS["MarinaBay"]` along the 19 turns, then
   `python scripts/osm_centerline.py MarinaBay` (needs `--offline` and the
   extract above). Compare the routed length with 4.93 km.
2. Width table by hand (lanes x 3.3 m is a start; the real track is closed and
   wider on the straights).
3. `seed_scene.py`, `ats-smooth`, `ats-bank` (flat circuit, so little banking).
4. Add the `BBOXES` entry, run `osm_layout.py MarinaBay --offline`, review
   the dossier with the definition of done in ADDITIONAL_TRACKS.md section 1.
5. Build the P1 props in section 4, then the gap props in section 5.
6. `CircuitStyle`: add a rule for the street circuit (no tree belts, quay rail
   and catch fence, night lighting) in `circuit_style.rs`.
7. `display_name` and the README table row in `content/tracks/default/README.md`.

## Sources

- Geofabrik, `malaysia-singapore-brunei-latest.osm.pbf`, OSM data of
  2026-10-08. (c) OpenStreetMap contributors, ODbL 1.0.
- [Marina Bay Street Circuit, Wikipedia](https://en.wikipedia.org/wiki/Marina_Bay_Street_Circuit):
  layout, turn names, layout changes.
- [TUMFTM/racetrack-database](https://github.com/TUMFTM/racetrack-database):
  track list checked 2026-10-09.
