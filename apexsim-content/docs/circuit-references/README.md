# Circuit references

Reference material for the real circuits the tracks in `tracks/default/` are modelled on: one dossier per
circuit, named by stem, describing the real venue down to building level. It covers which layout is
current and how earlier ones differ, every corner, the pit lane, pit building and paddock, and every
grandstand, bridge, tunnel, tower and building, placed by corner and side of the track. Each dossier
ends with its sources and lists the maps worth looking at.

These are for checking and fixing layouts and dossiers (`<Stem>.layout.json`, `MANUAL_*` in
`tools/osm_layout.py`), not runtime data. The real names here are source data. Screens still follow
[naming.md](../naming.md).

## Maps

`<Stem>.sources.json` lists the downloadable map files (official spectator/wayfinding maps first, then
circuit maps, then grandstand maps). The files are third-party material and are not checked in:
`maps/` is git-ignored. Fetch them with

```powershell
powershell -ExecutionPolicy Bypass -File docs/circuit-references/fetch_maps.ps1            # all
powershell -ExecutionPolicy Bypass -File docs/circuit-references/fetch_maps.ps1 -Stem Sudoku
```

The script saves each file to `maps/<Stem>/` and skips files already present. Some hosts refuse
scripted downloads (formula1.com images, a few circuit sites). The script reports those, and the
URL opens in a browser. Entries marked `"verified": false` are Wikimedia Commons paths worked
out from the file name. `maps/Zandervoort/2026-dutch-gp-wayfinding.pdf` is the official 2026 Dutch GP
wayfinding map, the model for the level of detail wanted everywhere.

## How the dossiers were made, and their limits

- They were researched from circuit, promoter, FIA and series pages, ticket-site grandstand guides,
  Wikipedia and racingcircuits.info.
- Map drawings were read for their text only. Positions come from text and page descriptions, not
  from looking at the maps, except at Zandvoort, where the map was read visually. Image-only PDFs
  (Bahrain FIA, Oschersleben, Indianapolis, Le Mans) still need reading by eye.
- No OpenStreetMap ids or surveyed coordinates are given. OSM and Commons could not be reached, so
  coordinates are the Wikipedia circuit centre plus an OSM map link.
- Anything a dossier could not confirm says "unconfirmed". The "Gaps" in the table are the open
  items worth checking against the maps or satellite imagery.

## Index

"In game" is the YAML's `metadata.length_m`; "real" is the current layout of the configuration the
track represents.

| Stem | Real circuit | In game | Real | Which layout the game has / what to check | Best map | Detail |
|---|---|---|---|---|---|---|
| [AlbertParkour](AlbertParkour.md) | Albert Park, Melbourne | 5.29 | 5.278 | Between 2021+ (5.278) and old (5.303): check for the old Lakeside Drive chicane (old T9-10, removed 2021) | F1 2026 detailed map, fan grandstand/gate maps | medium-high |
| [Armadillos](Armadillos.md) | Circuit of the Americas, Austin | 5.49 | 5.513 | Unchanged since 2012; 23 m short | F1 2026 map, grandstand/gate maps (no official PDF) | medium |
| [Bahrainless](Bahrainless.md) | Bahrain International Circuit, Sakhir | 5.39 | 5.412 | GP layout; shares tarmac with Outer/Endurance/Inner layouts | FIA 2024 circuit map (image PDF) | medium |
| [Barcelunatic](Barcelunatic.md) | Circuit de Barcelona-Catalunya | 4.64 | 4.657 | Length can't tell: final chicane = 2007-2022; tight La Caixa hairpin as well = pre-2021 | 2026 GP spectator map (PDF) | medium |
| [BrandsScratch](BrandsScratch.md) | Brands Hatch | 3.90 | 3.916 | GP circuit (2003+); Dingle Dell chicane gone since 2002 | Official grandstand map, corner/parking maps | medium-high |
| [ChillyVilleneuve](ChillyVilleneuve.md) | Circuit Gilles Villeneuve, Montreal | 4.35 | 4.361 | Unchanged; paddock building 2019; final chicane direction conflicts in sources | FIA 2026 circuit map (PDF) | medium |
| [Endianapolis](Endianapolis.md) | Indianapolis Motor Speedway | 4.02 | 4.023 | The 2.5 mi oval; road course documented too | IMS facility and gate PDFs (image-only) | medium-low positions, high oval geometry |
| [Gnocchi](Gnocchi.md) | Sochi Autodrom | 5.83 | 5.848 | 2014-2021 GP; GP loop dismantled 2023, so current imagery shows only the short layout | F1 2021 map, FIA media PDFs | medium |
| [HermanosRodriQueso](HermanosRodriQueso.md) | Autódromo Hermanos Rodríguez | 4.28 | 4.304 | 2015+ stadium section through Foro Sol; 24 m short | F1 2026 map, spectator/zone maps | medium |
| [Hockeyheimring](Hockeyheimring.md) | Hockenheimring | 4.56 | 4.574 | 2002 GP layout | Official 2024 site plan (PDF), labels every stand and building | medium-high |
| [Hungoverring](Hungoverring.md) | Hungaroring | 4.37 | 4.381 | Layout matches (2003+); buildings and grandstands all new 2024-26 | F1 2026 wayfinding map (PDF) | medium-high |
| [Interlaggos](Interlaggos.md) | Interlagos, São Paulo | 4.30 | 4.309 | Matches; pit exit rejoins at T3, pit-entry chicane since 2014 | 2026 GP sector map | medium |
| [Lemons](Lemons.md) | Circuit de la Sarthe, Le Mans | 13.62 | 13.626 | Matches; Arnage/Indianapolis 2016 and Porsche Curves 2017 reprofiles, start line moved 2018 | ACO 2026 car-park plan (PDF, whole circuit) | medium |
| [MandarinaBay](MandarinaBay.md) | Marina Bay, Singapore | 4.92 | 4.927 | 2023+ layout (floating-platform loop replaced by Raffles Avenue straight) | Official 2026 Circuit Park Map (PDF) | medium-high |
| [Monzarella](Monzarella.md) | Monza | 5.78 | 5.793 | Between 1995-99 (5.770) and 2000+ (5.793): check the first chicane is the single right-left | 2026 GP wayfinding map (PDF) | medium |
| [Mordschleife](Mordschleife.md) | Nürburgring Nordschleife | 20.76 | 20.832 | Matches; first corner after T13 is Sabine-Schmitz-Kurve (2021) | ADAC 24h site plan (2020, small) | medium |
| [MoscowMule](MoscowMule.md) | Moscow Raceway | 4.05 | 3.931 / 4.070 | Matches no real layout: DTM ran GP 9 (3.931 km), the full circuit is GP 2 (4.070 km) | Layout diagrams, access map | medium/low |
| [Nuerburgerring](Nuerburgerring.md) | Nürburgring GP-Strecke | 5.14 | 5.148 | 2002+ with Mercedes-Arena; start and control lines 263 m apart | FIA 2020 Eifel GP map (PDF) | medium |
| [Oskarsleben](Oskarsleben.md) | Motorsport Arena Oschersleben | 3.69 | 3.696 | Car layout with 90° T1 (2007+); pre-2007 maps show the bike T1 | Official overview map (PDF/JPG, image-only) | medium-low |
| [RedPullRing](RedPullRing.md) | Red Bull Ring, Spielberg | 4.31 | 4.318 | Matches; 4.326 km for 2025 is an FIA rescan, not a change | F1 2026 wayfinding map (PDF) | medium |
| [Shanghaied](Shanghaied.md) | Shanghai International Circuit | 5.43 | 5.451 | Unchanged since 2004 | Official 2026 grandstand map, FIA pit lane drawing | medium |
| [Shebang](Shebang.md) | Sepang International Circuit | 5.53 | 5.543 | Matches; T15 reverse camber since 2016. Hosted the relocated 2026 Bahrain GP | Grandstand map, F1 2026 map, SIC venue PDF | medium-high |
| [Shiverstone](Shiverstone.md) | Silverstone | 5.88 | 5.891 | Check pits and start at the Wing on Hamilton Straight (2011+) and the Arena infield (2010) | 2026 British GP spectator map (PDF) | medium-low |
| [Snorisring](Snorisring.md) | Norisring, Nuremberg | 2.28 | 2.300 | Unchanged since 1972; everything temporary except the stone tribune | Official 2026 site plan (PDF) | medium-low |
| [SpaFrankenchamps](SpaFrankenchamps.md) | Spa-Francorchamps | 6.99 | 7.004 | Between 2004-06 (6.976) and current: check the Bus Stop and La Source are post-2007 | 2026 GP wayfinding map (PDF) | medium |
| [Sudoku](Sudoku.md) | Suzuka | 5.80 | 5.807 | Matches (2003+); start line ~300 m ahead of finish line; real bridge at the crossover | Official 2026 F1 seat map, pit building PDF | medium-high |
| [YasMarinara](YasMarinara.md) | Yas Marina, Abu Dhabi | 5.52 | 5.281 | **Old 2009-2020 layout** (5.554 km, 21 turns): the 2021 rebuild removed the hairpin chicane and old T11-14 for banked T9 | FIA 2025 event notes and media kit (PDF) | medium-high |
| [Zandervoort](Zandervoort.md) | Zandvoort | 4.31 | 4.259 | **Old 1999-2019 layout** (4.307 km): no banking at T3/T14, old Hugenholtz position, old pit exit | 2026 Dutch GP wayfinding map (PDF, provided) | high |

## Refreshing

A dossier describes the venue as of October 2026. When a circuit changes (Monza rebuilds its pit and
media buildings from late 2026, Singapore's 2027 layout depends on the NS Square works), update its
dossier and its sources file.
