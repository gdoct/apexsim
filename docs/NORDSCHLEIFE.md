# Nürburgring Nordschleife

The 20.8 km Nordschleife on its own, from the T13 start/finish through the
Hohenrain chicane, clockwise: `content/tracks/real/Nordschleife.*`, lobby
name "Nürburgring Nordschleife", `track_id`
`7648a87e-a67d-43c2-b92a-84022cc3ca75`. The GP circuit stays
`Nuerburgring`.

## How it was built

Every other circuit's centerline is a GPS trace from the TUMFTM racetrack
database, which has no Nordschleife. This one is routed over
OpenStreetMap's `highway=raceway` ways instead, and everything downstream is
the standard pipeline (CLAUDE.md, "Refresh order").

1. **The extract.** The OSM map API refuses a box this size (50k-node
   ceiling) and was unreachable from the build machine anyway, so the
   extract was cut from the OSM planet file (`osm-pds` on AWS, the
   2026-09-21 ORC planet): every node inside `BBOXES["Nordschleife"]`, every
   way with a node inside, every relation with such a way or node as a
   member, written in the map API's JSON shape to
   `content/tracks/osm-cache/Nordschleife.0.json` (gitignored like every
   extract). `osm_layout.py` and `dem_fetch.py` then run with `--offline`.
2. **The centerline.** `scripts/osm_centerline.py Nordschleife` routes the
   lap waypoint by waypoint over the raceway graph (the waypoints are OSM's
   own `place=locality` nodes for each section, in race order), resamples
   it to 5 m nodes in the repo's frame (origin on the T13 line, +X along
   the course, +Y left), takes widths from the ways' `width` tags where OSM
   has them and 9 m elsewhere, and solves a minimum-curvature raceline.
3. **Elevation.** `scripts/dem_fetch.py Nordschleife --offline` writes the
   Copernicus GLO-30 sidecar on the dossier's fit, and
   `scripts/dem_elevation.py Nordschleife` derives the centerline's `z` from
   it (the YAML starts flat). The Nordschleife is wooded end to end, which is
   the case `dem_elevation.py` warns about: a surface model reads the
   canopy. Check the profile against the known one (about 300 m between
   Breidscheid and the Hohe Acht section) before trusting a re-derivation.
4. **Smoothing, banking.** `ats-smooth`, then `ats-bank` (no banking comes
   from OSM; see "Missing" below for the Karussell).
5. **The dossier.** `python scripts/osm_layout.py Nordschleife --offline`:
   corners from the named raceway ways and sections, stands, structures,
   crossings, woods, barriers, the surroundings, and the graffiti runs from
   `MANUAL_GRAFFITI`.
6. **The scene.** `scripts/seed_scene.py Nordschleife` starts the `.ats`
   (start line, grass bands, a curb at every detected apex and exit); then
   `ats-dress` lays the dossier and grooms, and `ats-export` writes the
   sidecars and the Unreal scene.
7. **Unreal** (Windows, not done by the pipeline above):

   ```powershell
   python content/props/_tools/gen_graffiti.py            # already committed; re-run only to change the art
   UnrealEditor-Cmd.exe game-unreal/ApexSim.uproject -run=ApexPropImport -kind=decal
   ./scripts/build_track_levels.ps1 -Track Nordschleife
   python scripts/build_track_catalog.py
   UnrealEditor-Cmd.exe game-unreal/ApexSim.uproject -run=ApexTrackCatalogSync
   ```

## Road graffiti

The fans' paint on the tarmac is a new scene layer, not a prop: `.ats`
`decals` (`ats.rs` `Decal`), baked by `ats-export` as road-hugging meshes
that follow the camber and the grade, drawn by the importer with a masked
`M_ApexDecal`. The pictures are 1024 × 512 PNGs under
`content/props/decal/graffiti/`, painted by
`content/props/_tools/gen_graffiti.py` (30 invented slogans, names, hearts,
arrows, flags). Where they lie is dossier data: `MANUAL_GRAFFITI` in
`scripts/osm_layout.py` gives runs of pictures (a section of road, the
images, the spacing), the dossier carries the expanded list, and
`ats-dress` owns every `graffiti/*` decal and re-lays them each run. See
docs/PROPS.md §9 for the format and for adding hand-painted art (Blender
texture paint or any image editor, 2:1 RGBA).

## Missing for a realistic Nordschleife

What the kit and the pipeline do not have yet, roughly in order of how
much it would show. Props are Blender assets in `content/props/<kind>/`
(docs/PROPS.md conventions: pivot, axes, material slot names).

| What | Why it matters | Suggested asset / work |
| --- | --- | --- |
| Double- and triple-height armco | The Nordschleife is lined almost end to end with stacked guard rail, often right at the road edge | `barrier/armco_4m_double`, `barrier/armco_4m_triple` (+ `_end` caps) |
| Barriers closer to the road | The groomer keeps rails 7 m (straights) / 14 m (corners) off the edge for the AI's sake; here they are 1-4 m | a per-circuit barrier profile in `groom.rs`, validated with the AI survey |
| Karussell concrete bowl | The inside of the Karussell is a steep concrete-slab gutter, not banked asphalt | a road profile feature (inside lane at ~30°) plus a `road_concrete` slab material; `surface_type: Concrete` on the YAML nodes is not read by the bake |
| Kilometre boards | Yellow boards every kilometre, the Ring's own way to say where you are | `board/km_marker` (text = km) |
| Numbered marshal posts | ~200 posts with their number on a board | `sign/marshal_post` exists; add a number face and let `dress` number them |
| Track-over-road bridges | The lap crosses the B257 on a bridge at Breidscheid (Adenauer Brücke) and passes under road bridges elsewhere | `bridge/road_underpass` (abutments, parapets) laid from OSM `bridge=yes` raceway segments |
| Rock cuttings | Ex-Mühle, Wehrseifen, Bergwerk run through cut rock faces | `misc/rock_face_8m` along the verge where the DEM slope is steep |
| Hedges and earth banks at spectator spots | Brünnchen, Pflanzgarten, Hohe Acht are banks of people behind hedges and wooden fences | `fence/hedge_4m`, `fence/wood_4m` exist; the dossier needs the spectator areas (OSM `tourism=viewpoint` polygons) |
| Camper and tent villages | The 24h weekend's camps line the Döttinger Höhe and Brünnchen | `vehicle/camper_van`, `attraction/tent_6m` exist; OSM `tourism=camp_site` areas feed `surroundings` |
| Touristenfahrten signage | Speed-limit, "no stopping", overtake-left signs, the T13 toll gates | `sign/road_sign_*` family (German road signs, flat panels on posts); `attraction/ticket_gate` exists |
| Yellow/black chevron boards | On the outside of the tight corners | `board/chevron_board` |
| Bridge-to-gantry gantry | The timing gantry at the end of the Döttinger Höhe | `bridge/timing_gantry` exists; add it as a `MANUAL_CROSSINGS` entry |
| Burg Nürburg and the Hohe Acht tower | The castle ruin on its hill inside the loop and the Kaiser-Wilhelm-Turm are the horizon's landmarks | `building/castle_ruin`, `building/stone_tower` + `MANUAL_LANDMARKS` |
| Graffiti on walls and armco | Tags on barrier faces and bridge abutments | a vertical decal (a textured quad on a barrier's face) — the road-decal material can be reused |
| Skid marks and oil-cement patches | Dark rubber streaks into the braking zones, pale cement dust after an incident | more `decal` sets (`skid/*`, `cement/*`), same bake and material |
