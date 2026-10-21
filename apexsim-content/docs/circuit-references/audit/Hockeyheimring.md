# Hockeyheimring (Hockenheimring GP, 2002 layout) — dressing audit

Sources: `circuit-references/Hockeyheimring.md`, README row; maps `hori-streckenplan-2024.pdf`
(official plan, rendered `gpd-grandstand-map-2019.jpg`; the `ats-dress --explain` output;
layout.json; MANUAL_STANDS (Mercedes-Tribune 2000-2250 outside), MANUAL_CROSSINGS
(Parabolika footbridge 1400).

Stations (layout corners + centerline heading change): line 0, lap 4561. T1 Ravenol-Kurve/
Nordkurve 221-403 (R, apex ~255; the 2024 plan labels T1 "RAVENOL-KURVE"), T2 Bernie-Ecclestone
770-905 (R), T3-T4 905-1046 (L), Parabolika 1046-1820, T6 Spitzkehre 2040-2125 (R), T7 2514-2543
(R), T8 2783-2842 (L), T9 2871-2901 (L), T10 2961-3021 (R), Mobil 1 3374-3528 (R), Sachs-Kurve
3724-3851 (L), T14-T16 3921-4131, Sudkurve 4181-4332 (R). Pit lane right, 865 m.

## Cells

| Column | Cell | Reason |
|---|---|---|
| S/F | dossier | Pit complex generated on the right (ok); "skipped building Boxengebaude/RAVENOL-Haus: in the pit complex" — the 200 m x 20 m roof-deck marquee over the garages (dossier "Pit building") is lost (rule/kit). Haupttribune laid from OSM as only 146 m (24.0 left); both maps draw it continuous A-H along the whole straight (~4400-160). **Nordtribune missing** (wraps the outside of T1, plan + grandstand map). Giant screens: none in the layout. |
| Landmark | dossier | No landmarks at all. Jim Clark Memorial (plan, outside T2 ~840 left) can be `misc/monument_obelisk` or `monument_statue_plinth` via MANUAL_FURNITURE. Baden-Wurttemberg-Center (7 levels) laid as 2 control_tower: ok-ish. MANUAL "Parabolika footbridge" (1400) is unsourced (its comment says so) and the official plan shows no bridge over the track, only pedestrian underpasses ("Unterfuhrung" symbols near the line, at T10-T11 and T12): remove it. |
| Stands | dossier | OSM "Osttribune" (2863 right, outside T8) is the plan's Osttribune and the grandstand map's "Mercedes" stand; the MANUAL "Mercedes-Tribune" 2000-2250 outside duplicates it at the Spitzkehre (8 x large bays, 298 m, where the plan has none; only temporary Spitzkehre stands for big events, `scaffold_10m`). OSM "Sudtribune" is laid right (inside) at 4282, 390 m x 185 m — the plan has it outside (left) the Sudkurve, wrapping onto the run-in from the Sachs loop; likely a whole-complex outline projected to the wrong side; check on the trace page (17 roofed bays may land in the infield/pit entry). Nordtribune missing. |
| Trees | dossier | Two woods (`mixed`), groom lays **4 trees** and no forest impostors. The dossier: Hardtwald, "dense pine and mixed woods" around the Parabolika and hairpin and the replanted old loop. Needs MANUAL_WOODS (needleleaved) both sides of the Parabolika 1046-2040 and round the T7-T10 return and T2. Metadata `longitude_deg: 8.48963` is ~5 km west of the venue (8.566); check it is not what the DEM/forest pass uses. |
| Barriers | rule | Groom: 1500 armco, 236 armco_fence, 1009 Tecpro, 60 tyres, **0 concrete**, though 96 OSM walls are mapped (Motodrom walls, pit wall). The Motodrom is concrete wall + high catch fence in front of the stands; only 236 fence cells because the stands sit 24-60 m back (SPECTATOR_RANGE_M 14). |
| Boards | ok | 485 hoardings, 21 distance boards, 7 corner signs, 15 marshal posts, camp site C3, car parks/Fahrerlager. Giant screens belong to S/F/Stands. |
| People | kit | Motodrom stands with crowd variants only; temporary standing areas (Spitzkehre I, Parabolika standing, Einfahrt Parabolika) and marshals. Wanted: standing crowd on banks/fences, marshals. |

## Positions from the maps

| Structure | Where | Side | Extent | Map |
|---|---|---|---|---|
| Nordtribune (A-C) | wraps the outside of T1 from the end of the straight to T1 exit, deep (3 tiers) | left | ~160-400, depth ~35-40 m | `hori-streckenplan-2024.pdf`, `gpd-grandstand-map-2019.jpg` |
| Haupttribune (A-H, incl. A-D centre block) | outside of the start straight, continuous from the Sudkurve exit to the Nordtribune | left | ~4350-160 (~370 m) | both |
| Sudtribune (A-D) + Sudtribune Oberrang | outside of the Sudkurve, continuing east along the run-in from the Sachs loop | left | ~4100-4350 | both |
| Innentribune (A-C) | east of the northbound leg into the Sachs-Kurve, inside the Motodrom | right | ~3500-3760 | both (OSM 3752 right: ok) |
| Osttribune (= 2019 "Mercedes") | west of the southbound leg after T8 | right | ~2840-2920 | both (OSM 2863 right: ok) |
| Mercedes-Tribune (MANUAL) | at the Spitzkehre, 2000-2250 left | — | remove, or replace by a temporary `scaffold_10m` Spitzkehre stand outside (left) ~2000-2080 | plan shows none |
| Giant screens | inside T1 (~260 right), at the line/podium, near T8-T9 (~2880), on the Parabolika before the hairpin (~1950 right) | as listed | `attraction/video_screen` via MANUAL_LANDMARKS kind `screen` | `gpd-grandstand-map-2019.jpg` |
| Jim Clark Memorial | outside T2 in the woods | left | ~840 | plan |
| Pedestrian underpasses | near the line, T10-T11, T12 | under | no prop | plan |
| Hotel Motodrom, Welcome Center, Home of Speed | behind the Haupttribune | left | ~4400-100 | plan (Home of Speed laid at 4381 left: ok) |

`hori-anfahrt-2023.pdf` and `hori-parken-camping-2026.pdf` (access/parking) not read; the plan's
inset covers parking and camps.

## Kit gaps
- Pit roof-deck marquee over a permanent pit building (`pit_building_roofdeck` exists for street circuits only) — the Boxendach hospitality terrace.

## Rule gaps
- Pit complex: the street-circuit roof deck is not offered to a permanent circuit with a roof terrace.
- Mapped walls never yield concrete; Motodrom fence missing where stands are set back past SPECTATOR_RANGE_M.
- A huge OSM stand outline (Sudtribune 390 x 185 m) is laid on the wrong side as one block: the stand pass trusts the centroid side of a wrapping ring.
## S/F dossier pass

In the tables:

- `MANUAL_STANDS`: Nordtribüne (100-400 outside covered).
- `MANUAL_LANDMARKS`: screen at 250.0 right.

Left, or for later columns:

- The unnamed OSM building at 300.3 left (352 x 95 m bbox, 3372 m2) is the Nordtribüne roof outline. If the dress pass lays it (the explain lists two unnamed buildings as "reaches across the road", likely this one and the Süd roof) it would sit on the new stand. It has no name, so MANUAL_STANDS cannot promote it; check on the trace page.
- The red U of three strokes just before the line on the pit side (gpd-grandstand-map-2019.jpg) reads as screens or the podium at the pit building; it lies in the garage zone and is not placed.
- The Haupttribüne is drawn continuous A-H from the Südkurve to the Nordtribüne. OSM gives LBBW-Haupttribüne 4505-90 and an "Südtribüne" whose front runs 4154-4505 at +33..+57 m (left of the road) although the layout records it side "right": the Süd/Haupt-south complex as one ring. It is an OSM stand and cannot be moved here; the side mismatch is a rule gap (wrapping-ring side by centroid) for the Stands column.
- Boxendach roof-deck marquee over the garages is lost to the pit complex (kit/rule gap). The MANUAL "Mercedes-Tribüne" at the Spitzkehre and the "Parabolika footbridge" belong to the Stands and Landmark columns (audit: remove both).

- The new screen's landmark takes the unnamed 352 m building outline at 300.3 m left out of the dossier, which is the Nordtribüne's own roof (and was skipped as reaching across the road).

## Landmark dossier pass

In the tables:

- `MANUAL_CROSSINGS`: the Parabolika footbridge, authored by judgement, is gone; the 2024 Streckenplan shows only underpasses there.

Left, or for later columns:

- The Jim Clark memorial (845 left) would need a statue piece of its own size.

## Stands dossier pass

In the tables:

- OSM's wrapping Südtribüne outline is left out (`@4282.2`) and laid again from its road-facing edge as two covered runs, Südtribüne 4150-4305 and Haupttribüne E-H 4310-4503 (R13 for this track); the Spitzkehre Mercedes-Tribüne is gone (the 2024 plan has none; the 2019 map's is the Osttribüne); the Innentribüne open.

Left, or for later columns:

- The Osttribüne's roof; an unnamed roof outline.
