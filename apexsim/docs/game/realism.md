# Realism reference sheet

The game set beside real frames at Spa, Monza and the Nordschleife, and what
is furthest off ranked by how much of the frame it covers. The ranking sets
the order of the realism packages (roadmap 53 and 55 and the candidates under
*Not yet scoped*). Package 29 decides *what* stands where; this sheet and
package 55 decide how each thing *looks*.

## The sheet

- **Game shots**: the `g8-*` runs in `scripts/verify/runs.yml`, all sunny at
  14:00 with the HUD hidden: `g8-spa-gt3`, `g8-monza-gt3`,
  `g8-nordschleife-gt3` (the Caravan GT3 in a GT3 field: a cockpit burst on
  lap 1, held poses, the TV director, the chase `near` rung),
  `g8-spa-formula`, `g8-monza-formula`, `g8-nordschleife-formula` (the
  SF-26's cockpit and T-cam) and `g8-nuerburgring-pit` (the Nordschleife
  layout has no pit lane, so its pit frame is the GP circuit's).
- **Reference frames**: the owner's, in `out/screenshots/` (onboards,
  broadcast stills and photos; not ours to ship, so never committed).
- **Pairs**: `python scripts/verify/realism_sheet.py` puts each reference
  beside its game shot in `out/realism/` (`index.html`, one JPG a pair, the
  game shots copied beside them so a later run does not move them). The pair
  list is in the script.

The six views and what stands in for each:

| View | Spa | Monza | Nordschleife |
|---|---|---|---|
| Cockpit, GT3 | Eau Rouge onboard | onboard, and one in the pit lane | two onboards (Hatzenbach, graffiti) |
| Cockpit, formula | none yet | a render from another sim (not real) | a T-cam onboard |
| Chase | fan video behind Eau Rouge | broadcast rear shot | onboard-style rear shot in the woods |
| TV trackside | broadcast at Eau Rouge | low broadcast camera on a straight | low trackside, an aerial at Kesselchen |
| Pit lane | from above | from the cockpit | the GP pit lane at eye height |
| Grandstand straight | the finish line, the main straight | none yet | none (the layout has no stands) |
| Wooded section | aerial over Raidillon | park trees, pines | aerial, a bend in the forest |

Still wanted: a formula onboard at Spa and at Monza (a real one), and a
Monza grandstand straight.

## The ranking

Share of the frame is read off the pairs: the part of the game frame where
the gap shows, in the views where it shows.

| # | Gap | Where it shows | Share | Taken by |
|---|---|---|---|---|
| 1 | **Forest cover.** Wooded circuits stand in open grass with scattered trees: Monza's park is a lawn with a few trees (the Lesmo woods at eye height and from above), the Nordschleife at 8 km and 9.6 km is open meadow where the real track runs in a corridor through dense forest, Spa's hills are bare grass with conifer stands. The real frames are 50-90 % trees. | woods, aerial, TV, chase backgrounds, cockpit side windows | 40-80 % of woods and aerial frames, 20-40 % of the rest | the woods query and stand planting ([track-pipeline.md](../../../apexsim-content/docs/track-pipeline.md)); 55's vegetation for how each tree looks |
| 2 | **Cockpit interior.** The procedural rig is flat, untextured grey panels lit like the outside: no carbon, no switch panels, screens or harness, a bare cage, no hands on the wheel, and none of the real interior's dark against a bright windscreen. | every cockpit frame, both classes | 55-65 % of cockpit frames | 53 (hands), new: cockpit interior materials and lighting |
| 3 | **Ground and run-off materials.** Grass is one uniform green with tufts dotted on it; run-off paint is clean and saturated (Spa's red-yellow bands); the road is clean grey with dark square patches; no gravel texture, no worn verges. | every outside view | 20-40 % | 55 (materials), the road-surface candidate |
| 4 | **Buildings, stands and people.** Buildings are white boxes with window grids (the Spa paddock block at Eau Rouge, the pit buildings); pit-lane furniture is flat red blocks; stands have no roof structure or crowd density; spectators are cards. | TV, grandstand, pit lane | 10-30 % of those frames | 55 (pit building, people) |
| 5 | **Atmosphere.** Valley haze at the Nordschleife washes the 8 km bend and the aerial grey; the sky is a uniform saturated blue. The real frames have haze only at distance and a paler, lit sky. | Nordschleife views, every sky | 10-60 % | built: an 800 m haze layer, the fog off the sky, the atmosphere's aerosol ([conditions.md](../server/conditions.md)) |
| 6 | **Cars.** Clean, plastic-looking paint, one livery per model across the field; no dirt or rubber pickup. | chase, TV | 10-20 % | liveries dealt across the field and dirt over a session built; flake left (roadmap 60) |
| 7 | **Close trackside kit.** Armco, fences and the pit wall read as simple geometry: no bevels, fixings or wear. | trackside low shots, pit lane | 5-15 % | 55 (close families) |

Outside the ranking (not a look but what the camera shows): the TV director
cuts to trackside cameras with foliage or armco filling the lens
(`g8-monza-gt3` #17, `g8-nordschleife-gt3` #11, #12, #14); in
[follow-ups.md](../follow-ups.md), Rendering.

The owner agreed the ranking on 2026-10-31; it is work-plan items G13, G14 and
G16 with G9 and G11 (forest cover, the first, and the atmosphere, the fifth,
are built).
