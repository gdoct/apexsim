# Interlaggos (Interlagos) dressing audit

Sources read: dossier `circuit-references/Interlaggos.md`; maps `gpsp-sector-map-2026.webp`
(official 2026 sector map, every stand and club), `gpgt-grandstand-map-2025.webp`
(schematic, old letters Q/R); `f1-circuit-map.webp` and `commons-track-map.svg` not needed
for positions. Explain the `ats-dress --explain` output; `Interlaggos.layout.json`;
`MANUAL_STANDS["Interlaggos"]` (D, R, G, T4); circuit style TROPICAL.

Corner stations (layout corners): S do Senna 292-327, Curva do Sol 592-781, Reta Oposta
781-1367, Descida do Lago 1367-1515, Ferradura 1978-2090, Laranjinha 2090-2216, Esse
2279-2405, Pinheirinho 2405-2571, Bico de Pato 2571-2872, Mergulho 2872-3096, Junção
3135-3236, Café 3356-3430, Subida dos Boxes 3588-3778, Arquibancadas 3894-4155; lap 4303.
Lap is anticlockwise: every public stand on the perimeter is on the **right**; pit lane left.

## Cells

| Column | Cell | Reason |
|---|---|---|
| S/F | dossier | OSM stands on the straight are there (unnamed 58.6 m at 77 = B, "M" 162 m at 190, unnamed 100 m at 4272 = A's top end), but laid uncovered (`stand (unnamed): 5 x bay_10m_large`, `stand M: 5 x bay_10m_large`) where the dossier says B and M are covered and B carries the Champions/Tower Club on top and the giant screen. Pit Stop Club (covered, facing the grid) missing. Secondary kit need: the pit building has the Paddock Club on an upper floor over the boxes (dossier "Pit building"), the bake lays single-row garages. |
| Landmark | - | No unique structure: the dossier's buildings are stands, clubs and the kart track. The infield lake is mapped as water. |
| Stands | dossier | MANUAL D runs 291-500 and swallows H's place; H (covered, T2-T3 outside) missing; G (1150-1368) sits at the braking end where the 2026 map has the long G bar from R to mid-straight; "T4" (1368-1607, 275 m, the old V stand that may not exist in 2026) stands where the map has the short L-shaped Porto Bank stand at the end of the Reta Oposta; A, the largest stand, is only OSM's 100 m at 4272 instead of the ~700 m bar from Café to Arquibancadas. Orange Tree Club, Grand Prix Club and the Heineken Village Estrela platform (infield) missing. |
| Trees | ok | Six OSM woods (all `mixed`), the biggest (32,000 m², the infield of the Reta Oposta, the dark-green patch on the 2026 map) is present; TROPICAL flora adds palms (26 `palm_oil`, 17 `palm_ornamental`). Planting is thin (~80 trees + 31 impostors over ~56,000 m² of woods): worth a density look, but no source contradicts the species. |
| Barriers | ok | `armco_4m_fence` dominates (1190) as an urban circuit with crowds on every side wants; 51 tyre modules, 500 Tecpro. The dossier says nothing on barriers. Check item: the outside of Arquibancadas and the pit straight is a concrete wall with catch fence in photos; if confirmed, a mapped wall line (dossier). |
| Boards | ok | 417 hoardings, 12 corner signs from the 14 named corners (2 skipped in the pit zone), 14 marshal posts, 17 light panels. |
| People | kit | Covered and uncovered stands all round the perimeter, the Heineken Village infield crowd (Gramado lawn, Estrela platform), marshals. No figure kit; wanted: crowd on the uncovered A and G bleachers, a standing crowd on the Gramado lawn, marshals. |

## Positions from the maps

| Structure | Where | Side | Extent | Map |
|---|---|---|---|---|
| A (largest, uncovered) | from Café along Subida dos Boxes and Arquibancadas to the start of the straight | right (outside) | ~3420-4220 (one long bar; OSM has only 4221-4321) | gpsp-sector-map-2026 (A), gpgt-2025 (A, long bar along the last curve and straight) |
| Unlabelled red stand | outside of Junção / Café, just before A begins | right | ~3250-3400, short | gpsp-sector-map-2026 (red block, not in its legend) |
| B (covered, numbered sectors 1-6, Tower Club on top) | start of the straight opposite the grid | right | as OSM (~45-105); mark covered | gpsp-sector-map-2026 (B) |
| Pit Stop Club (covered) | between B and M | right | ~105-130, short | gpsp-sector-map-2026 |
| M (covered, numbered 1-8) | towards T1, in the braking zone | right | as OSM (110-270); mark covered | both |
| D (covered) | outside of the S do Senna | right | ~290-410 (shorten from 291-500) | gpsp-sector-map-2026 (D) |
| H (covered) | between the S and Curva do Sol, facing T2 and the pit exit | right | ~420-570 | gpsp-sector-map-2026 (H) |
| R (covered, raised) | outside of Curva do Sol, start of the Reta Oposta | right | as MANUAL (592-782) | both |
| G (uncovered; Q on gpgt) | along the Reta Oposta after R | right | ~800-1200 (move from 1150-1368) | gpsp-sector-map-2026 (G long bar), gpgt-2025 (Q next to R) |
| Arquibancada Porto Bank (covered, L-shaped) | end of the Reta Oposta, outside the Descida do Lago entry | right | ~1330-1430, L round the corner | gpsp-sector-map-2026 |
| Remove/shrink: "T4" stand | T4-T5 exit | right | 1368-1607 | not on the 2026 map |
| Orange Tree Club (stand + lounge) | infield beside the start of the straight / pit entry, facing Laranjinha | left of the straight (infield) | ~4200-4260, ~60-80 m in from the pit lane | gpsp-sector-map-2026, gpgt-2025 |
| Grand Prix Club (VIP building) | infield behind the paddock, toward Laranjinha / Pinheirinho | infield | ~150 m in from the straight at ~4280; near Laranjinha 2156 | gpsp-sector-map-2026 |
| Heineken Village Gramado + Estrela platform (star-shaped raised deck) | infield between Mergulho/Junção and Subida | left (infield) of 3000-3500 | ~150 x 150 m area | gpsp-sector-map-2026 |
| Premium Paddock Club / Interlagos Club | inside T1 by the pit exit / infield | left | ~250-330 | gpgt-2025 only (older) |

## Kit gaps

- `building/pit_building_tiered`: garages with a hospitality floor above (Paddock Club over
  the boxes); shared with Hungoverring and Lemons.
- `attraction/viewing_platform_star` (or a generic raised `viewing_deck`): the Estrela
  platform. A `fanzone_stage` + `marquee_peak_12m` can stand in for the rest of the village.
- `grandstand/club_on_roof`: a glazed box on top of a stand (Tower Club over B). Could also be
  a `hospitality_3f` placed on the stand, so possibly not a kit gap.

## Rule gaps

- Tooling (osm_layout.py): an OSM-traced stand's `covered` comes only from its OSM tags
  (`covered=yes` / `roof:shape`), and a MANUAL_STANDS entry cannot rename or re-roof an OSM
  stand (it only replaces a same-named *structure*; `NO_OSM_STANDS` drops all OSM stands).
  B and M need either a per-stem OSM-stand override (name + covered) or to be re-authored as
  MANUAL_STANDS with the OSM ones dropped. B (58.6 m, depth 23.5) and M (depth 21.4) are under
  the 25 m always-roofed depth, so they come out open.
