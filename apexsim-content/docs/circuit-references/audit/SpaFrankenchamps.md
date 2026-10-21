# SpaFrankenchamps audit (Spa-Francorchamps)

Sources read: `circuit-references/SpaFrankenchamps.md`, README row, maps
`2026-belgian-gp-wayfinding.pdf` (rendered whole + S/F and east zooms; north points left on
it), the `ats-dress --explain` output, `SpaFrankenchamps.layout.json`,
`MANUAL_STANDS["SpaFrankenchamps"]` (Les Combes, Bruxelles, Pouhon, Bus Stop; no other
MANUAL_* entry), `.ats` by stamp. Not read: `csf-access-plan.pdf`, the parking map, the F1
webp (the wayfinding map already places every stand).

Lap 6988 m. YAML corners: La Source 392, Eau Rouge 1057, Raidillon 1205, Kemmel 1600,
Les Combes 2409, Malmedy 2660, Bruxelles 3071, Speaker's Corner 3318, Double Gauche (Pouhon)
3968, Fagnes 4565, Campus 4949, Paul Frere 5174, Blanchimont 6006, Chicane 6747. Pit lane
right (inside the straight), entry ~6765, exit ~547 after La Source; matches the dossier.

## Cells

| Column | Cell | Reason |
|---|---|---|
| S/F | dossier | Pit building is the export's ("skipped building Stand F1: in the pit complex"). Gold 1 is OSM "Tribune F1" (st 49 L, 90 m) but laid unroofed although every Gold stand is covered (dossier). Gold 9/9bis between G1 and La Source are missing. "Tribune Bus Stop" (MANUAL, 6650-6820 **right**) is on the pit-entry side and loses a prop to the pit zone; the map puts G11 and G6 on the **left** between the chicane exit and the line. Live screens along the straight absent. |
| Landmark | dossier | No landmarks laid. Map: Ferris wheel + second fan zone outside Les Combes (Green Area), F1 Fanzone with its stage outside the Eau Rouge descent, Business Tower (CC) / Ardennes Club (AC) / Eau Rouge Champions Club (ER) round the downhill. Kit exists (`big_wheel`, `stage`, `tower`, `screen`). Surprise: explain "skipped building Salons de l'Eau Rouge: stands on the run-off" drops the real building at the foot of Eau Rouge (st 1047 R). |
| Stands | dossier | Laid: Silver 1, Endurance (G2), RX (Fanzone stand area), Raidillon, Les Combes (S7), Bruxelles (S4), Pouhon (S3), Bus Stop, 3 small OSM stands at La Source (G7/G8). Missing per map: Gold 3 (Kemmel), Gold 5 (Raidillon right), C1-C4 (Malmedy descent), Speed Corner stand, Verstappen/Fan Area + S5/S6 (after Pouhon), 17/27 (at Paul Frere, not Blanchimont as the dossier text has it), G9/G11/G6. Raidillon (covered, 2022) laid unroofed. Bronze 1-6 GA banks have no family (rule gap). |
| Trees | dossier | Rich already: 2677 forest impostors, ~1200 trees, 30 woods. But 25 of 30 woods are `mixed`/`broadleaved`; only 3 needleleaved, and the Ardennes round Les Combes, Pouhon/Fagnes and Stavelot is largely spruce plantation (dossier "Ardennes forest"). Set leaf `needleleaved` on the big woods there (e.g. 984,184 m^2 mixed at 4988 L, 294,334 mixed at 3879 R, 157,938 mixed at 1756 L). |
| Barriers | ok | Groom: 1562 armco, 1226 armco_fence, 1065 tecpro, 119 tyres; matches a modern permanent circuit with Tecpro on the fast bends and fences at the spectator areas. 2022 gravel is a surface matter, not this column. |
| Boards | ok | 453 hoardings, 14 distance boards (5 braking corners), 14 corner signs, 20 marshal posts, 29 light panels. |
| People | kit | Bronze GA banks 1-6 (Kemmel, Pouhon, Blanchimont, Campus), Pouhon GA hill, the fan zones, gates. No figure kit. |

## Positions from the maps (`2026-belgian-gp-wayfinding.pdf`)

Sides are in driving direction.

- **Gold 1** (= OSM Tribune F1, st ~5-95 L): set covered.
- **Gold 9 + Gold 9bis**: left of the straight between G1 and La Source, ~st 110-300 L, depth ~14, covered.
- **Gold 11 + Gold 6 + PMR Chicane**: left, between the Bus Stop chicane exit and the line, ~st 6810-6950 L, depth ~14, covered. Replace "Tribune Bus Stop" (right).
- **Gold 7 / 7bis / 7ter / Gold 8**: outside (left) of La Source, round the hairpin from its entry to the apex, ~st 340-440 L (OSM's three small stands at 402-407 L cover part; extend).
- **Silver 1, PMR Source, Gold 2**: left of the downhill after La Source: Silver 1 laid (655 L), Gold 2 = "Tribune Endurance" (761 L), ok.
- **Fanzone Grandstand**: left, at the foot of Eau Rouge (~st 930-1020 L), with the F1 Fanzone (hatched) and its **stage** behind it ~100-180 m out. Landmark `stage`.
- **Gold 4**: left on the Raidillon climb = "Tribune Raidillon" (1247 L); set covered.
- **Gold 5**: right of the top of Raidillon/start of Kemmel, ~st 1250-1350 R, depth ~14, covered.
- **Gold 3**: left of the Kemmel straight, ~st 1380-1480 L.
- **Bronze 1** (GA bank): left of Kemmel, ~st 1500-2300 L.
- **Silver 7**: left at the end of Kemmel before Les Combes, = "Tribune Les Combes" (2320-2470 L). ok.
- **Ferris wheel + Combes fan zone**: outside Les Combes beyond the C stands, ~st 2450-2550 L, 150-250 m out. Landmark `big_wheel`.
- **C1-C4** (pink grandstands, four blocks): left (outside) of the Malmedy descent to Bruxelles, ~st 2560-2950 L, depth ~14.
- **Silver 4**: outside (left) of the Bruxelles hairpin = "Tribune Bruxelles" (2990-3140 L). ok.
- **Speed Corner grandstand** (pink, long): left of the run from Bruxelles to Pouhon, ~st 3180-3450 L.
- **Bronze 3**: right of the same run, ~st 3150-3850 R. **Silver 3**: right at the Pouhon entry (~3820-3900 R) = "Tribune Pouhon" roughly. **Bronze 4**: right, outside Pouhon ~3950-4100 R.
- **Verstappen + Fan Area (pink) + Silver 5 + Silver 6**: right (outside) of the run from Pouhon down to Fagnes, ~st 4080-4480 R.
- **17/27 + Bronze 5**: left (outside) of Campus/Paul Frere, ~st 4950-5200 L (the dossier has 17-27 at Blanchimont; the map puts it here).
- **Bronze 6**: left of Blanchimont's approach, ~st 5750-6150 L.
- **Business Tower (CC), Ardennes Club (AC), Eau Rouge Club (ER)**: right (inside) of the downhill from the pit exit to Eau Rouge, ~st 700-1050 R. CC is the new tower (landmark `tower`), AC/ER `hospitality_3f`.
- **Live screens**: ~30 on the map; the biggest by the pit straight (inside, ~st 6950 and ~200), La Source outside, Eau Rouge both sides, Les Combes, Bruxelles, Pouhon, Blanchimont, Paul Frere.
- **Karting track**: inside Paul Frere (OSM building at 5428 R). ok.

## Kit gaps

- People figures (banks, fan zones, gates). No structure-level gap.

## Rule gaps

- GA banks (Bronze 1-6, Pouhon hill) need a bank family / spectator-area entry (same gap as Zandervoort).
- "Stands on the run-off" drops Salons de l'Eau Rouge at the foot of Eau Rouge (st 1047 R, 26 m out): check `runoff_reach` there; the building is real and stands where the dossier puts it.
- Gold stands (covered in reality) come out unroofed because OSM carries no covered tag: needs a dossier `covered` override per OSM stand (MANUAL table keyed on a name).
## S/F dossier pass

In the tables:

- Removed: Tribune Bus Stop.
- `MANUAL_STANDS`: Gold 9bis (100-160 left covered); Gold 9 (165-235 left covered); Gold 6 (6810-6865 left covered); Gold 11 (6885-6945 left covered).
- `STAND_COVER`: Tribune F1 covered.
- `MANUAL_LANDMARKS`: screen at 6790 left.

Left, or for later columns:

- The map's screens by the straight itself (two black posts on the left edge between Gold 1 and Gold 9, ~95 and ~240 L, and two labels in the paddock) are left out: a screen on the left edge would stand between the road and the stands, and the paddock ones sit in the pit complex. Trap: a screen at 245 L dropped the OSM pit building row "Stand F1" (drop_structures_under_landmarks takes any authored landmark within half the 312 m building's length of its centre, either side of the road). Harmless for the dress (the building is left to the pit complex) but it changes the dossier.
- Gold 7 / 7bis / 7ter / 8 round La Source (~340-440 L) are turn 1, left to the Stands column; the Bus Stop loses its seating-map stand.
- Map stand sizes are icons, not to scale: spans are good to ~30 m.

## Landmark dossier pass

In the tables:

- `MANUAL_LANDMARKS`: the Combes fan zone wheel (2470 left, 150 m) and the F1 Fanzone stage (1080 left, 110 m), 2026 wayfinding map.

Left, or for later columns:

- The Eau Rouge Champions Club (OSM "Salons de l'Eau Rouge", dropped as on the run-off) as `hospitality_3f` through `MANUAL_STRUCTURE_ASSETS` on the next full extract.
- The CC Business Tower (about 950 right): re-read now that R19 takes only the outline a landmark stands on.
