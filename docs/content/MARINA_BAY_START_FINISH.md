# Mandarina Bay: props still needed for the start/finish area

Brief for whoever authors the props (a Blender/`bpy` agent working to
`docs/content/PROPS.md` and the `mb_*` helpers in
`scripts/content/props/marina_common.py`). Reference: the night aerial of the
2025 pit straight (a roughly 900 m curved straight, pit building on the
left, one giant stand on the right).

## What is in the game now (do not remake)

Generated pit complex (`garage_6m`, `box_kit`, `pit_wall_6m`, `pit_wall_plain_6m`),
`start_gantry`, `timing_gantry`, `hoarding_3m/6m`, `tecpro_2m`, `bay_10m*`
stand bays and caps (`_crowd` twins), `catch_fence_6m`, `lamp_arm_twin`,
`raintree_l`, `tent_6m`, `video_screen`, `fanzone_stage`, `camera_tower`,
`control_tower`, `pit_speed_limit`, `pit_exit_light`, `marshal_post`,
`corner_sign`, `building_pit_street` (built, not placed: it collides with the
generated garages), the ten `skyline_*` and the Marina Bay landmarks.

## What the photograph has that we lack

1. A pit building whose roof is a black deck 200 m long carrying giant white
   letters, with terraces, a zig-zag scaffold staircase and a glowing balloon
   row along its edge.
2. One continuous tiered grandstand the length of the straight: a full
   multi-storey stand with a canopy edge picked out in LED lines (blue and
   lime green), a catch fence with posts on the wall in front of it, and a
   hospitality deck behind it.
3. Blue/white and yellow **painted edge bands** along both sides of the
   straight (the yellow band is wide), brand text painted on the wall base.
4. An overhead sponsor banner gantry across the straight, plus a small timing
   tower with a scaffold stair at the chequered line.
5. Globe lamps: white spheres on thin poles all along the walkway, and
   tethered glowing balloons (white and orange) over the pit roof and paddock.
6. The pit lane: wall-top gantry, overhead light truss, team signage, garage
   number boards, a walkway with trees and lamps behind the garages.
7. Hospitality marquees and a lit LED fan zone behind the stand.

## Props to author

Priority: P1 = the straight looks wrong without it, P2 = what makes it
read as a night race, P3 = polish. Sizes are real-world metres. All follow
the kit conventions (pivot, axes, modules on 2/4/6 m, `mb_*` materials with
`*_emis` textures for anything lit; emissive slot names below are what the
Unreal night pass already drives: `mb_lit_panel` (plain emissive),
`ferris_lights*`, `floodlight_lamp`, `led_panel`/`led_screen`, `gantry_lamp`).

### Buildings and stands

| kind | asset | size (L x D x H) | description | pri |
|---|---|---|---|---|
| building | `pit_building_roofdeck` | 200 x 34 x 26 | The pit building as a deck: flat black roof with a raised parapet, two terraces stepped down to the lane side (-Y), glazed hospitality band, service stair towers at both ends, roof masts. Roof carries the letters slot below. Replaces `building_pit_street` here (that one has its own service bays). Centred, front on -Y. | P1 |
| board | `roof_wordmark_block` | letter 4 x 0.6 x 6 each, a 12-letter set 60 m | Free-standing white letters on a roof deck with a lit red/white edge, brand text from `text` (texture, not geometry, if easier: one 40 x 6 m panel on stilts, `board_brand` slot, `mb_lit_panel` rim). Real names are trademarks: the text is the dossier's `display_name`. | P2 |
| misc | `stair_zigzag_scaffold` | 14 x 5 x 8 | Zig-zag steel stair with handrails and orange step lights, links two roof levels. | P3 |
| grandstand | `street_stand_tier_10m` | 10 x 26 x 17 | One bay of a tall street-circuit stand: 18 rows of seats on a scaffold frame, rear service deck, rear and side balustrades, front edge lit by two LED strip slots (`stand_led_blue`, `stand_led_green`, emissive). Needs the crowd twin (`_crowd`, same cards as `bay_10m_crowd`). | P1 |
| grandstand | `street_stand_tier_end` | 4 x 26 x 17 | End cap: stair tower, open side, same LED slots. | P1 |
| grandstand | `street_stand_tier_10m_roof` | 10 x 26 x 22 | Same bay with the canopy (truss roof with lit underside). | P2 |
| grandstand | `street_stand_deck_10m` | 10 x 14 x 9 | Low premium deck stand (glazed, balcony) for the pit-exit and turn stands. | P3 |

### Overhead and track furniture

| kind | asset | size | description | pri |
|---|---|---|---|---|
| bridge | `banner_gantry` | 20 m span (scales with `span_m`), 7 m | Skewed truss gantry with one big double-sided banner board (`board_brand` slot, text = brand) and an LED underside strip. Sits at the chequered line beside `start_gantry`. | P1 |
| misc | `finish_tower_scaffold` | 4 x 4 x 7 | Timing/marshal tower at the line: scaffold cabin with stair and rails, camera, flag mast. | P2 |
| light | `lamp_globe_pole` | 0.6 x 0.6 x 6.5 | Thin pole with a 0.8 m white sphere (`globe_lamp` slot, emissive, warm white). The walkway and fan-zone rows. | P1 |
| light | `lamp_balloon_tether` | 1.6 x 1.6 x 9 | Tethered light balloon, 1.6 m, white or orange variants (`balloon_lamp_white/orange`), thin tether line. | P2 |
| light | `pit_light_truss_6m` | 6 x 1.2 x 1.2 | Overhead light truss module for over the pit lane and walkway, lamp bar underneath (`floodlight_lamp`). Tiles on 6 m. | P2 |
| board | `led_ribbon_3m` | 3 x 0.15 x 0.9 | LED advert strip (`led_panel` slot, brand from `text`) to sit on top of Tecpro/walls in front of a stand. | P2 |
| fence | `catch_fence_post_lit_6m` | 4 x 0.5 x 6.5 | Like `catch_fence_6m` but with an angled head and a lit rail along the top (`mb_lit_panel`). | P3 |
| barrier | `spectator_handrail_4m` | 4 x 0.1 x 1.1 | Galvanised handrail for the walkways and roof decks. | P3 |
| sign | `pit_entry_board` | 3 x 0.3 x 4.5 | Overhead-mounted blue "PIT" board on two posts with arrow. | P3 |
| sign | `garage_number_board` | 1.5 x 0.1 x 0.8 | Number plate for a garage door; `text` = 1-24 (reuse the `T_marker_<n>` pipeline). | P3 |
| pit | `pit_wall_gantry_6m` | 6 x 2 x 3.2 | Pit-wall top: timing screen, team table, headset rack, overhead awning. Replaces the plain `pit_wall_6m` team stand on the lane side. | P2 |

### Fan zone behind the stand

| kind | asset | size | description | pri |
|---|---|---|---|---|
| attraction | `marquee_peak_12m` | 12 x 8 x 6 | White peaked marquee with lit panels (`mb_lit_panel`), open front on -Y. | P2 |
| attraction | `led_wall_stage_16m` | 16 x 6 x 9 | Stage with a tall LED back wall (`led_screen`) and truss. | P3 |
| misc | `walkway_planter_4m` | 4 x 1.2 x 1.2 | Paved planter with a rain-tree sapling, for the paddock walkway. | P3 |

## Decals and textures (not meshes)

The bake paints these onto the road, so the prop agent only needs the
images (`content/props/decal/<set>/*.png`, same pipeline as the Nordschleife
graffiti: `ats-dress` lays them, `ats-export` bakes them road-hugging).

| image | size | use |
|---|---|---|
| `edge_yellow_blue.png` | tiling 8 x 4 m | The wide yellow band beside a blue band along the straight's edges. |
| `pit_exit_blue_green.png` | 12 x 8 m | Painted lane markings at the pit exit. |
| `wall_base_brand.png` | 4 x 0.6 m | Brand text on the base of the wall in front of the stand. |
| `finish_chequer_wide.png` | 12 x 2 m | The chequered line (the bake paints a plain one today). |
| `pit_lane_digits.png` | atlas | Lane numbers and speed-limit roundels. |

## Hooks I (not the prop agent) will have to add

So the list is not the whole job: the dresser needs rules for the start
straight (a `MANUAL_STANDS` family switch to `street_stand_tier_*`, the pit
building replacing the generated garage block, the banner gantry at station
0, the globe-lamp row along the walkway, the decals), and the client needs
night-pass wiring for the new emissive slots (`stand_led_*`, `globe_lamp`,
`balloon_lamp_*`). Each prop therefore wants its emissive slots named exactly
as above, and every asset the unit list calls "tiles" must tile with its own
`_end` where it has a cap.
