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

## Where it stands (2026-09-25)

- **Lap**: 20 763 m after smoothing (the official figure is 20 832 m),
  4 154 nodes, 9 m road, tightest radius 22.8 m (the Karussell), 292 m of
  relief, steepest grade 15%. The line is 75 m up the T13 straight from
  OSM's "Start-Ziel T13" node, because the fallback grid is laid straight
  back from the line for 64 m and the node sits right after the bend from
  Hohenrain.
- **Dossier**: fit rmse 0.75 m, centerline covered 100%; 43 named corners
  (every section: Hatzenbach ... Karussell ... Döttinger Höhe), 33 woods,
  123 barrier runs, 2 road bridges, Burg Nürburg, 135 graffiti.
- **Scene**: about 9 500 vangrail modules, 1 200 tyre modules, 270 signs,
  29 600 trees; re-dressing is byte-identical.
- **Walls check**: 2 m of the lap edge open (`check_walls.py --openings`).
- **AI survey** (`SURVEY_TRACKS=Nordschleife`, 10 cars, 180 s): LMP2 and
  GT3 fields 1.7-1.8 car-seconds of contact, 10-13 off the road; the F1
  field has one car that leaves the road at the Hatzenbogen and is pinned
  against the rail for 82 s (the AI's recovery from a wall, as at
  Oschersleben; the close rails make it likelier here).
- **No pit lane**: the lane OSM maps at T13 merges into the road across
  the start area and its walls stood on the racing line; the circuit's
  style lays none (the endurance races pit in the GP paddock).
- **Not done here**: the Unreal side (`ApexPropImport -kind=decal` and the
  new kit meshes, `build_track_levels.ps1 -Track Nordschleife`, the catalog
  sync) needs a Windows machine with the engine; none of the C++ changes
  were compiled in this session.

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
docs/PROPS.md §10 for the format and for adding hand-painted art (Blender
texture paint or any image editor, 2:1 RGBA).

## The circuit's own style

`track-editor/core/src/circuit_style.rs` gives the Nordschleife its own
grooming rules (every other circuit keeps the old ones):

- **Vangrail.** German guard rail instead of the kit's UK armco: the double
  `vangrail_4m` in the corners, the three-high `vangrail_4m_triple` on the
  fast stretches, `vangrail_4m_fence` where people stand behind it,
  `vangrail_end` closing a run. It stands 3 m off the road on straights and
  4.5 m in corners (7 m / 14 m elsewhere); authored run-off still pushes it
  back.
- **Forest.** The tree belt starts 8.5 m from the road (just behind the
  rail) and runs to 110 m, 6-11 trees per 12 m cell a side with no
  clearings, inside the OSM woods — against 22-90 m and 3-6 elsewhere.
- **German signs** instead of braking boards and hoardings: red-white
  chevron boards round the outside of every corner tighter than 160 m, a
  kilometre board every kilometre, bend warnings before the corners under
  90 m and a danger sign before the ones under 40 m, and the
  "Überholen nur links" board every 3 km.
- **Burg Nürburg** stands on its hill inside the loop (`MANUAL_LANDMARKS`,
  anchored on its OSM position, laid as `building/castle_ruin`).

Tight rails change what the AI hits: check the AI survey
(`SURVEY_TRACKS=Nordschleife`) after touching the distances.

## Missing for a realistic Nordschleife

Done since the first cut: the vangrail, the forest, the chevron and
kilometre boards, the bend/danger/overtaking signs and the castle (above).
What the kit and the pipeline do not have yet, roughly in order of how
much it would show. Props are Blender assets in `content/props/<kind>/`
(docs/PROPS.md conventions: pivot, axes, material slot names).

| What | Why it matters | Suggested asset / work |
| --- | --- | --- |
| Karussell concrete bowl | The inside of the Karussell is a steep concrete-slab gutter, not banked asphalt | a road profile feature (inside lane at ~30°) plus a `road_concrete` slab material; `surface_type: Concrete` on the YAML nodes is not read by the bake |
| Numbered marshal posts | ~200 posts with their number on a board | `sign/marshal_post` exists; add a number face and let `dress` number them |
| Track-over-road bridges | The lap crosses the B257 on a bridge at Breidscheid (Adenauer Brücke) and passes under road bridges elsewhere | `bridge/road_underpass` (abutments, parapets) laid from OSM `bridge=yes` raceway segments |
| Rock cuttings | Ex-Mühle, Wehrseifen, Bergwerk run through cut rock faces | `misc/rock_face_8m` along the verge where the DEM slope is steep |
| Hedges and earth banks at spectator spots | Brünnchen, Pflanzgarten, Hohe Acht are banks of people behind hedges and wooden fences | `fence/hedge_4m`, `fence/wood_4m` exist; the dossier needs the spectator areas (OSM `tourism=viewpoint` polygons) |
| Camper and tent villages | The 24h weekend's camps line the Döttinger Höhe and Brünnchen | `vehicle/camper_van`, `attraction/tent_6m` exist; OSM `tourism=camp_site` areas feed `surroundings` |
| Bridge-to-gantry gantry | The timing gantry at the end of the Döttinger Höhe | `bridge/timing_gantry` exists; add it as a `MANUAL_CROSSINGS` entry |
| More German signage | Speed limits, "no stopping", the T13 toll gates | more `board/de_*` signs; `attraction/ticket_gate` exists |
| The Hohe Acht tower | The Kaiser-Wilhelm-Turm on the Hohe Acht is the other skyline landmark | `building/stone_tower` + `MANUAL_LANDMARKS` |
| Graffiti on walls and armco | Tags on barrier faces and bridge abutments | a vertical decal (a textured quad on a barrier's face) — the road-decal material can be reused |
| Skid marks and oil-cement patches | Dark rubber streaks into the braking zones, pale cement dust after an incident | more `decal` sets (`skid/*`, `cement/*`), same bake and material |
