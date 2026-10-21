# BrandsScratch (Brands Hatch GP) — dressing audit

Sources read: `circuit-references/BrandsScratch.md`, README row; maps `msv-grandstand-map.jpg`,
`oversteer48-grandstands.jpg` (aerial), `oversteer48-gp-parking-map.jpg` (venue map with key);
the `ats-dress --explain` output; `BrandsScratch.layout.json`; `osm_layout.py` (BrandsScratch has
**no** MANUAL_* entries at all, only BBOXES and CORNER_DISPLAY).

Stations: line 0, lap 3.90 km. Corners (layout.json): Paddock Hill 217-421, Hailwoods Hill 421-565,
Druids 565-686, Graham Hill 783-893, Cooper Straight 893-1167, Surtees 1167-1244, Pilgrims Drop
1324-1570, Hawthorn Hill 1570-1954, Hawthorns 1954-2119, Westfield 2330-2458, Dingle Dell
2458-2737, Sheene 2737-2845, Stirlings 2941-3037, Clearways 3312-3400, Clark Curve 3416-3674,
Brabham Straight 3709-217. Pit lane right (inside), 560 m.

## Cells

| Column | Cell | Reason |
|---|---|---|
| S/F | dossier | Pit complex comes from the pit lane on the right (ok). The OSM stands are all there but unroofed: the aerial (`oversteer48-grandstands.jpg`) shows Paddock Hill (grey roof) and Pit Straight (red roof) covered; layout has `covered: false`. Explain: "skipped building Media Center: in the pit complex" — the Nigel Mansell / Media Centre over the pits (dossier "Start/finish") is lost; the export lays single-storey garages only (rule/kit, see below). Brabham-Stewart building laid as 4 x hospitality_3f (ok). |
| Landmark | dossier | Footbridge at 515.6 (Hailwood Hill) -> truss_bridge: ok. The two OSM road bridges (1709.6 Hawthorn Hill, 3303.5 Stirlings-Clearways), both drawn "BRIDGE" on `oversteer48-gp-parking-map.jpg`, come out as `tyre_bridge` (explain "bridge (unnamed): truss_bridge, 2 x tyre_bridge") because kind `road` falls to the default; Brands has no tyre arch. Needs `CROSSING_KINDS["BrandsScratch"] = {"road": "overpass"}`. Unnamed OSM `tower` at 3653 left becomes a control_tower beside Clark Curve exit; no source names a tower there (check on trace page; drop or keep). |
| Stands | dossier | 5 OSM stands laid (Pit Straight, unnamed 39.5, Desire Wilson, Paddock Hill, Hailwoods). Missing roofs (above). Missing spectator areas: Clearways raised viewing area (outside Clearways), South Bank (outside Cooper Straight) as `spectator_bank_10m`; Director's Terrace. |
| Trees | dossier | 12 woods all `leaf: mixed`; groom lays 164 poplar + 157 conifer among 260 broadleaf. Aerial shows Kent deciduous woodland (Westfield Wood, Dingle Dell, Druids infield, the big wood inside Pilgrims/Surtees). Set woods to broadleaved (MANUAL_WOODS or a per-stem leaf override). |
| Barriers | rule | Groom: 377 armco, 1373 armco_fence, 498 Tecpro, **0 tyres, 0 concrete**. Dossier: Druids exit "tyre barrier", Paddock Hill gravel then barrier; Brands lines its slow corners (Paddock, Druids, Graham Hill) with tyre walls in front of armco. `barriers::decide` gives Tecpro wherever run-off >= 15 m at R<80 m. 1373 fence cells come from the 36 mapped OSM fences (ok, Brands has debris fencing widely). |
| Boards | ok | 341 hoardings, distance boards, 18 corner signs, 19 marshal posts, light panels, car parks Lower Paddock / Southbank / GP Paddock, 22-house village outside Sheene (the housing estate). Campsite (map: "CAMPING" south of H gate) not laid: minor. |
| People | kit | Spectator banks everywhere on the GP loop and South Bank, Clearways viewing area, Paddock Hill bank; marshal posts at every corner. Wanted: standing/seated spectators on banks and fences, marshals (orange) at posts. |

## Positions from the maps

- **Pit Straight Grandstand** — roofed, outside (left) of Brabham Straight just past the line; layout has it at 3876.9 left, 24 m. `msv-grandstand-map.jpg` draws it about as long as Paddock Hill (~45 m) just after the start flags toward Paddock Hill; `oversteer48-grandstands.jpg` shows a red-roofed block. Set covered, ~40-45 m, check station on the trace page (map suggests ~0-60 rather than ~3865-3890).
- **Paddock Hill Grandstand** — roofed (grey roof in aerial), outside, ~150-200. Layout covered=false.
- **Hailwoods Grandstand** — small roofed stand at the foot of Hailwoods Hill, outside (left), ~410; layout ok except roof.
- **MotorSport Vision Centre** (key 1, `oversteer48-gp-parking-map.jpg`) — big building outside Clark Curve, left, ~3480-3560; explain "skipped building MotorSport Vision: stands on the run-off". It is the largest building on the venue side; step it back (structure offset) so it survives.
- **Medical Centre** (key 13) — inside McLaren/Clearways, near 3250-3300 right (a laid unnamed clubhouse at 3246 left may be it on the wrong side; check).
- **Kentagon** (key 8) — hexagonal bar behind Paddock Hill (193.7 left) laid as clubhouse: ok.
- **Road bridges** — "BRIDGE" at Hawthorn Hill (~1709) and between Stirling's and Clearways (~3303): concrete overpasses, not tyre arches.
- **Pedestrian tunnels / vehicle tunnel** marked under Paddock Hill (blue dotted at ~240) — no prop needed.
- **Giant screens**: none marked on any map.

## Kit gaps
- Pit-building upper storey (media centre over the garages) — stands in for the Nigel Mansell Centre; shared with Montreal, Sochi, Hockenheim (see rule gap on the pit complex).

## Rule gaps
- Crossing kind `road` defaults to `tyre_bridge` (dress.rs crossing asset match `_ => "tyre_bridge"`); a road over the track is an `overpass_concrete`. Per-stem fix is CROSSING_KINDS; the default itself is the better fix (also hits Montreal 1920.5, Sochi 4786.3).
- `barriers::decide` lays no tyre walls on a club circuit whose slow corners have them (Tecpro chosen by run-off).
- Run-off skip drops the MSV Centre whole instead of stepping it back.
## S/F dossier pass

In the tables (`STAND_COVER`): the Pit Straight, Paddock Hill and Hailwoods
Hill stands roofed, as the aerial shows them (OSM tags none covered); Desiré
Wilson open. Every pit-straight stand is OSM's, on the side the maps show, so
nothing is added or moved. No map marks a big screen.

Left:

- The OSM Pit Straight stand is 24 m against 40-45 m on the aerial; it is a
  stand, so no table entry lengthens it.
- The unnamed OSM stand at 39.5 m left is most likely the covered Desiré
  Wilson blocks under the Brabham-Stewart overhang; unnamed, it cannot be
  roofed by name.
- The Media Centre over the pits is the pit complex's (kit gap 2).

## Landmark dossier pass

In the tables:

- `DROP_SCANNED_LANDMARKS`: OSM's only tower (3653.1 left, node 1304656417) is a phone mast, no longer laid as race control.
- The two road crossings (1709.6, 3303.5) are real (the parking map marks both BRIDGE) and are concrete overpasses by R4.

Left, or for later columns:

- The MSV Centre: R14.

## Stands dossier pass

In the tables:

- Two banks: South Bank (910-1150 left, by the Southbank car park) and the Clearways viewing area (3335-3415 left). The OSM stands are right.

Left, or for later columns:

- The GP-loop banks, Paddock Hill bank and the Director's Terrace.
