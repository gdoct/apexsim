# HUD modding: components and data points

The race HUD is drawn from files. Every panel is a **component**: a folder
holding one `component.json` that says where the panel sits and what it
draws. What it draws comes from **data points**, named values the game
publishes every frame (`car.speed_kph`, `lap.last_s`, `standings`, ...). The
game never draws a panel by name, so a player can move, restyle, replace or
add any of them without C++ or the Unreal editor: edit a file, run
`apexsim.hud.Reload` in the console, and the HUD is rebuilt in place.

Players who only want to move panels use the HUD editor (Settings > Gameplay
> HUD layout), which writes `custom/layout.json` on top of the components' own
placement.

This file is the modding reference, and it is checked: the automation test
`ApexSim.Hud.Data.Documented` fails unless every data point, list, list field
and expression function appears here in backticks.

## Code

- `content/hud/default/<id>/component.json`: the shipped components;
  `content/hud/custom/` the player's own (gitignored but for its README).
- `game-unreal/Source/ApexSim/Public/Hud/`: `ApexHudComponent.h` (loading,
  `ApexHud::Scenes()`), `ApexHudExpression.h` (the expression language),
  `ApexHudData.h` (`FApexHudData`, `FApexHudInputs`, `FApexHudMemory`,
  `FApexHudPreview`, `ApexHudData::Build`), `ApexHudDataSubsystem.h`,
  `ApexHudLayout.h` (`FApexHudLayout`, `ApexHudLayouts`, `ApexHudPlace`).
- `game-unreal/Source/ApexSim/Public/UI/ApexHudWidget.h` (the host) and
  `UI/ApexHudEditorWidget.h` (the editor).
- Tests: `game-unreal/Source/ApexSim/Private/Tests/HudTests.cpp`.

## Where components are read from

```
content/hud/
  default/            the shipped HUD (one folder per component)
    car_state/component.json
    standings/component.json
  custom/             the player's own: read after default/
    my_relative/component.json   <- a new component
    standings/component.json     <- replaces default/standings
    layout.json                  <- the HUD editor's arrangement
```

| Source | When |
|---|---|
| `content/hud` in the repo | editor builds (`play_editor.ps1`) |
| `Hud/` beside `ApexSim.exe` | a packaged game (`build_game_standalone.ps1` copies `Hud/default` and an empty `Hud/custom`; `-SkipHud` leaves it out) |
| `-ApexHudDir=<dir>[+<dir>]` | added on top, last one wins |

Each directory is read as `default/` then `custom/` (or, with neither, as a
folder of components). A later folder replaces an earlier one with the same
component id. To hide a shipped component, write
`custom/<id>/component.json` holding `{ "enabled": false }`.

Shipped components: `track_info`, `race_state`, `status`, `minimap`,
`standings`, `timing`, `pedals`, `damage`, `car_state`, `mirror`, `pit_stop`;
off until added in the editor: `relative`, `speed_gear`, `conditions`; while
watching: `spectator_tower`, `spectator_driver`, `spectator_controls`,
`spectator_replay`; the `hotlap_watch` scene: `hotlap_header`,
`hotlap_corner`, `hotlap_timing`, `hotlap_car`, `hotlap_controls`.

## Quick start

1. Copy `content/hud/default/standings` to `content/hud/custom/standings`.
2. Change `"max": 5` to `"max": 10` in the copy.
3. In a race, run `apexsim.hud.Reload` in the console.

`apexsim.hud.Data [filter]` prints every data point with its current value
(`apexsim.hud.Data tyre`). A component that fails to load is left out and the
reason shows on screen and in the log; anything suspicious that does not stop
it loading (an unknown key, a data point the game does not publish) is a
warning in the log (`LogApexSim`).

## The component file

JSON with two allowances: `//` and `/* */` comments, and trailing commas.

```jsonc
{
  "name": "Speed",                 // shown in messages
  "description": "Big speedo",     // optional
  "region": "bottom",              // where on screen, see below
  "order": 10,                     // order within the region, lowest first
  "margin": [0, 0, 0, 40],         // space around it
  "float": false,                  // true: placed alone at the region's corner
  "visible": "=car.present",       // optional: hide the whole component
  "enabled": true,                 // false: leave it out (how custom/ hides a default)
  "default_enabled": true,         // false: shipped off; the HUD editor offers to add it
  "scene": "",                     // optional: belongs to a scene
  "root": {                        // the element tree
    "type": "panel", "padding": [16, 8], "background": "surface",
    "children": [
      { "type": "text", "font": "display", "size": 64, "text": "{fmt(car.speed)}" }
    ]
  }
}
```

### Regions

Nine regions: `top-left`, `top`, `top-right`, `left`, `center`, `right`,
`bottom-left`, `bottom`, `bottom-right`. Components in the top and bottom
bands sit side by side (lowest `order` first, left to right); in `left`,
`center` and `right` they stack top to bottom. Every region keeps 56 px from
the sides and 30 px from the top and bottom. A `"float": true` component is
placed alone at its region's corner, offset by its `margin`, and moves nothing
else (the virtual mirror is one). Keep `center` empty in anything meant for
racing: it is the driver's sight line. Once a player moves a component in the
HUD editor, `layout.json` pins it and that wins over the region.

### Scenes

A component with a `"scene"` belongs to a view that replaces the HUD rather
than adding to it. There is one, `hotlap_watch` (a watched hotlap, see
[sessions](../server/sessions.md)): while `hotlap.active` is true only that
scene's components show; outside it they never do. A `custom/` component with
a shipped id replaces it as anywhere; your own with `"scene":
"hotlap_watch"` joins them. The HUD editor lays out only the ordinary HUD.

## Elements

Keys on every element:

| Key | Meaning |
|---|---|
| `type` | one of the types below |
| `children` | a list of elements (containers only) |
| `width`, `height` | fixed size in pixels (either may be left out) |
| `margin` | space around the element inside its parent |
| `halign` | `left`, `center`, `right`, `fill`: across a column, or within a panel |
| `valign` | `top`, `center`, `bottom`, `fill`: across a row, or within a panel |
| `fill` | in a row or column, share of the spare room; an expression (`"=pit.service_fuel_share"`) sizes it every frame, which is how a bar is cut into segments |
| `visible` | `true`, `false` or an expression; a hidden element takes no room |
| `repeat` | draw the element several times, see "Repeating" |

| Type | What it is | Its own keys |
|---|---|---|
| `row` | children side by side | |
| `column` | children top to bottom | |
| `stack` | children on top of each other | |
| `panel` | a filled box around its children | `padding`, `background`, `outline`, `outline_width`, `radius`, `direction` (`column`, `row`, `stack`) |
| `text` | a line of text | `text`, `font` (`body`, `display`, `mono`), `size`, `tracking`, `bold`, `upper`, `color`, `justify` |
| `label` | a caption: `text` in small muted mono capitals | as `text` |
| `rect` | a filled rectangle (lights, segments, dots) | `color`, `radius`, `outline`, `outline_width` |
| `bar` | a progress bar filled to `value` (0 to 1) | `value`, `color`, `background`, `direction` (`right`, `left`, `up`, `down`) |
| `spacer` | empty room; `"fill": 1` pushes its neighbours apart | |
| `divider` | a hairline rule | `vertical` |
| `keycap` | a key hint chip, `ESC MENU` | `text` (fixed) |
| `image` | a PNG from the component's folder | `file`, `color` (tint) |
| `minimap` | the circuit outline with a dot per car | `color` (the player's dot) |
| `mirror` | the virtual rear-view mirror, while the setting is on | `width`, `height` |

A panel with exactly one child (not a repeat) places it by the child's
`halign`, `valign` and `margin`; with more it lays them out by `direction`
(a column unless told otherwise). Fonts are the menu's: `display` the
condensed heading face, `body` the running text (the only one with a bold),
`mono` numbers and captions. `tracking` is letter spacing in thousandths of an
em. `margin` and `padding` take a number, `[horizontal, vertical]` or
`[left, top, right, bottom]`, in pixels at 1080p (the HUD scales with the
screen).

### Repeating

- `"repeat": 30` (or `{ "count": 30 }`) draws the element 30 times, with
  `index` 0 to 29 inside. The rev counter is one rect repeated 30 times, lit
  when `index < round(car.rpm_fraction * 30)`.
- `"repeat": { "list": "standings", "max": 5, "focus": "is_local" }` draws one
  copy per row of a list (at most `max`), with `item.<field>` reading the row
  and `index` its number from 0. `focus` names a true/false field: the window
  scrolls to keep the first row where it is true in the middle. Copies with no
  row are hidden.

## Values and expressions

An attribute is fixed or an **expression** worked out every frame.

- `text`: plain text with `{expression}` holes: `"LAP {lap.display}/{lap.limit}"`;
  `{{` and `}}` are literal braces.
- `color`, `background`, `outline`: a colour, fixed. `visible`, `bold`:
  `true`/`false`, fixed. `value`, `fill`: a number, fixed.
- Any attribute starting with `=` is an expression:
  `"color": "=car.drs_open ? 'live' : 'border'"`, `"value": "=car.throttle"`.

The language: literals (`12`, `1.5`, `'text'` or `"text"`, `true`, `false`,
`null`); data points by name, and `item.<field>` / `index` in a repeat;
`+ - * / %` (`+` joins text when either side is text; division by zero is 0);
`== != < <= > >=`; `&& || !` or `and or not`; `condition ? a : b`; the
functions below. A data point the game cannot fill this frame is `null`: it
reads as false, 0 and empty text, and `has(x)` tells it apart. Expressions are
compiled when the component loads (a broken one is a load error) and have no
side effects.

| Function | Result |
|---|---|
| `if(c, a, b)` | `a` when `c` is true, else `b` |
| `switch(x, k1, v1, k2, v2, ..., default)` | the `v` paired with the first `k` equal to `x`; a last odd argument is the default (`null` without one) |
| `has(x)` | whether `x` is not `null` |
| `min(a, b, ...)`, `max(a, b, ...)` | smallest, largest |
| `clamp(x, lo, hi)` | `x` held between `lo` and `hi` |
| `abs(x)`, `floor(x)`, `ceil(x)` | |
| `round(x)`, `round(x, digits)` | to whole numbers, or to `digits` places |
| `fmt(x)`, `fmt(x, digits)` | as text with that many decimals (0 by default); `—` for `null` |
| `fmt_time(s)` | a lap time, `1:32.104`; `--:--.---` for none |
| `fmt_split(s)` | a split, `27.431`; `--.---` for none |
| `fmt_gap(s)`, `fmt_gap(s, false)` | a gap, `+0.412` (unsigned with `false`); `—` for none or over 999 s |
| `fmt_delta(s)` | a signed delta, `-0.250`; `—` for `null` |
| `fmt_clock(s)` | a countdown rounded up: `23:45`, `1:23:45` past an hour, `0:00` when out; `--:--` for `null` |
| `upper(t)`, `lower(t)` | case |
| `str(x)`, `num(x)` | as text, as a number |
| `mix(c1, c2, t)` | colour `c1` blended toward `c2` by `t` (0 to 1) |
| `ramp(x, x0, c0, x1, c1, ...)` | the colour at `x` along the stops, held past either end: `ramp(damage.front, 0, 'text_disabled', 25, 'accent', 60, 'error')` |
| `alpha(c, a)` | colour `c` with opacity `a` |
| `rgb(r, g, b)`, `rgb(r, g, b, a)` | a colour from 0-255 sRGB channels, opacity 0 to 1 |

**Colours** are `#RRGGBB` / `#RRGGBBAA` (sRGB) or a palette name:
`background`, `surface`, `surface_hover`, `border`, `accent`, `on_accent`,
`text` (also `text_primary`), `text_secondary`, `text_muted`,
`text_disabled`, `live` (green), `error` (red), `focus`, `white`, `black`,
`transparent`, and the timing colours `session_best` (purple),
`personal_best` (green), `tyre_cold` (blue). The names keep a component in
step with the game's look (`Hud/ApexHudValue.cpp`).

## Data points

Refreshed every frame while a race view is open. Times are seconds,
temperatures °C, distances metres. "Local" is the car the HUD is about: the
player's, or while watching, the car on screen (every `car.*`, `lap.*`,
`tyre.*`, `sector.*` is then that car's). *null* marks when a value can be
`null`; a value from a field an older server does not send is *null* too.

### HUD and session

| Name | Meaning |
|---|---|
| `hud.full` | the HUD detail setting is "All" |
| `hud.time_s` | seconds since the race view opened, for blinking and fades |
| `hud.imperial` | the units setting is imperial |
| `hud.speed_unit` | `KM/H` or `MPH` |
| `hud.gamepad` | the last input came from a pad: key hints should show its buttons |
| `session.track_name` | the circuit's display name |
| `session.car_name` | the local car's display name |
| `session.mode` | `race`, `qualifying`, `practice`, `hotlap`, `countdown`, `lobby`, `sandbox`, `demo_lap`, `replay` |
| `session.mode_name` | the same as the menus show it ("Race") |
| `session.is_race`, `session.is_hotlap` | shortcuts on the mode |
| `session.conditions` | the sky in words, "Light rain · 21:30 · wind 12 km/h" (*null* outside a session) |
| `session.weather` | "Sunny", "Light rain", ... (*null* outside a session) |
| `session.clock` | the time of day the session was set at, "21:30" (*null* outside a session) |
| `session.air_temp_c` | the air, when the host set it (*null* when left to the weather) |
| `session.wind_kph` | the wind, when the host set it (*null* when left to the weather) |
| `net.ping_ms` | heartbeat round trip, refreshed every two seconds (*null* before the first) |

### The sky now

The server sends the live sky with every telemetry frame (see
[conditions](../server/conditions.md)). Every one but `sky.live` and `sky.wet`
is *null* without it (an older server, a showcase stream); the `session.*`
figures then stand for the sky. Percentages are whole numbers.

| Name | Meaning |
|---|---|
| `sky.live` | the server reports the sky now |
| `sky.clock` | the time of day as the clock runs, "15:30" |
| `sky.clock_s` | the same, seconds after midnight |
| `sky.weather` | the weather the forecast has reached; rain and cloud ease in behind it |
| `sky.rain` | rain falling, percent of a downpour (light rain is 50) |
| `sky.cloud` | cloud cover, percent |
| `sky.road_water` | water on the road, the lap's mean, percent of what a downpour leaves on a flat road; over 100 is standing water |
| `sky.wet` | the road counts as wet (5% water or more); false without a sky |
| `sky.air_c`, `sky.track_c` | the air and the asphalt |
| `sky.wind_kph` | the wind this moment, gusts included |
| `sky.wind_rel_deg` | where the wind blows *toward*, degrees clockwise from the local car's nose, -180 to 180: 0 a tailwind, ±180 a headwind (*null* in a calm or with no car) |
| `sky.wind_from` | where it comes from, seen from the local car: "ahead", "ahead-left", "the left", ... "behind" (*null* as above) |
| `sky.rubber` | rubber on the racing line, the lap's mean, percent: 50 the track cars are set up on, 0 green, 100 rubbered in |
| `sky.time_scale` | how fast the clock runs (0 frozen, 1 real time, 24 a day an hour) |
| `sky.next_weather` | the forecast's next weather (*null* while the sky holds) |
| `sky.next_in_s` | seconds of session until it arrives (*null* as above) |

### Watching a race

Set while the player watches rather than drives: the menu's backdrop race
full screen, a live session from the browser, a saved replay, or a watched
hotlap (see [spectator](spectator.md)). The `spectator_*` components show only
then; `standings`, `race_state`, `track_info`, `status` and `mirror` hide.

| Name | Meaning |
|---|---|
| `spectate.active` | watching: the player has no car, the local car is the one on screen |
| `spectate.live` | the race is a live session (not a recorded showcase, file or demo) |
| `spectate.source` | `showcase`, `file`, `demo`, `replay`, `live` or `hotlap` (*null* when not watching) |
| `spectate.camera` | `TV`, `CHASE`, `ONBOARD` (*null* when not watching) |
| `spectate.auto` | the TV director chooses the car |
| `spectate.tower_mode` | the timing tower's column, stepped with T: `interval`, `gap`, `last`, `best`, `tyres` |
| `spectate.waiting` | watching, between two races (no cars yet) |
| `hotlap.active` | a watched hotlap: one AI car lapping alone; turns the `hotlap_watch` scene on |
| `replay.active` | a saved replay is playing |
| `replay.time_s`, `replay.duration_s` | how far into it, and how long it runs (*null* outside a replay) |
| `replay.progress` | `replay.time_s` over `replay.duration_s`, 0 to 1 |
| `replay.rate` | playback speed: 0.25, 0.5, 1, 2 or 4 |
| `replay.paused` | paused |
| `replay.ended` | at its end, holding the last frame |

### Race order and gaps

The order, the places and every gap here and in `standings` are taken at
most twice a second and held in between, so two cars side by side do not
swap places on screen every frame.

| Name | Meaning |
|---|---|
| `race.position` | the local car's place, from 1 (*null* without a car) |
| `race.car_count` | cars in the session |
| `race.leader_lap` | the leader's lap, as `lap.display` (*null* with no cars) |
| `race.timed` | the race runs to a clock rather than a lap count |
| `race.time_left_s` | a timed race's clock: all of it on the grid, counting from green, 0 once out (*null* in any other session) |
| `race.final_lap` | the lap a timed race ends on, the leader's lap when the clock ran out (*null* until then, and outside a timed race) |
| `gap.ahead_name`, `gap.behind_name` | the drivers either side (*null* at either end) |
| `gap.ahead_s`, `gap.behind_s` | the gap to them at the local car's speed (*null* when the circuit's length is unknown) |

### Laps and timing

Lap times, sectors and track limits are the server's.

| Name | Meaning |
|---|---|
| `lap.current` | the lap counter as sent (0 on the grid) |
| `lap.display` | the lap to show, held at the race distance on the cool-down lap |
| `lap.limit` | race distance in laps, 0 when there is none (hotlap, practice, a timed race) |
| `lap.laps_left` | laps to the flag, the one in progress included; in a timed race estimated from the last lap until the clock runs out (*null* without a race distance, or in a timed race before a lap is in) |
| `lap.final` | the lap in progress is the last |
| `lap.time_s` | the lap in progress |
| `lap.invalid` | the lap in progress has been struck for leaving the track |
| `lap.last_s`, `lap.last_invalid` | the last completed lap, and whether it was struck |
| `lap.best_s` | the best legal lap this session |
| `timing.delta_s` | up (negative) or down on the reference lap at this point (*null* until a legal lap is in) |
| `timing.reference_s` | the reference: the quickest legal lap the HUD has watched |
| `timing.session_best_s` | the session's fastest legal lap |
| `timing.session_best_name` | who set it |
| `timing.session_best_is_local` | the local car set it |
| `timing.optimal_s` | the local car's best sectors added up |
| `sector.current` | the sector the car is in, from 1 |
| `sector.count` | sectors in a lap (3 unless the track names its own) |

### The corner

From the server's `TrackCorners` (sent with a watched hotlap): the
[track guide](../content/track-guide.md)'s stops, numbered from the start line
and named with the display name, never the real one. They describe the corner
the car is in or, between corners, the next one; all *null* (and
`corner.inside` false) without corners or a car.

| Name | Meaning |
|---|---|
| `corner.number` | its number, from 1 |
| `corner.name` | its name, *null* when the dossier has none |
| `corner.label` | what to show: the name, else "Turn N" |
| `corner.count` | corners in the lap |
| `corner.direction` | `left` or `right`, the way its main turn goes |
| `corner.inside` | the car is between its entry and its exit |
| `corner.distance_m` | metres to its entry (0 inside it) |

### The car

| Name | Meaning |
|---|---|
| `car.present` | there is a local car (or a car on screen while watching) |
| `car.index` | its car index |
| `car.driver_name` | its driver's name, as the standings show it |
| `car.pit_stops` | stops made since the HUD first saw it (*null* without a car) |
| `car.retired` | out of the race: a damage zone has reached 100% |
| `car.speed_kph`, `car.speed_mph` | speed |
| `car.speed` | speed in the player's units |
| `car.gear`, `car.gear_text` | gear as a number (-1 reverse, 0 neutral) and as shown (`R`, `N`, `1`...) |
| `car.rpm` | engine speed |
| `car.rpm_max` | the rev counter's scale: the car's rev limiter, or the highest revs seen |
| `car.rpm_fraction` | `car.rpm / car.rpm_max`, 0 to 1 |
| `car.redline_rpm` | the redline from car.toml (*null* when unknown) |
| `car.throttle`, `car.brake` | pedals, 0 to 1 |
| `car.steering` | steering, -1 to 1 |
| `car.in_garage` | parked in the hotlap garage |
| `car.on_track`, `car.colliding` | |
| `car.headlights` | the headlights are on |
| `car.finish_position` | classified place once finished, else 0 |
| `car.drs_allowed`, `car.drs_open` | the DRS flap may open here / is open |
| `car.tow` | share of drag the slipstream saves, 0 to 1 |
| `car.x`, `car.y`, `car.z`, `car.yaw_deg` | position in the track frame (+X along the start straight, +Y left) and heading |
| `car.station_m` | distance along the lap |
| `pit.in_lane`, `pit.limiter`, `pit.servicing` | in the pit lane / on the limiter / stopped at the box |
| `pit.service_s` | seconds of service left |
| `pit.autopilot` | the server drives the car along the pit route (the player's input is ignored) |
| `pit.exit_closed` | the pit exit light is red |
| `pit.held` | the car is waiting at the red exit light |
| `pit.box` | the car's box, from 1, as its last stop named it (*null* until it has stopped) |
| `fuel.liters` | fuel in the tank |
| `fuel.per_lap` | what the last whole lap burnt (*null* until one is measured) |
| `fuel.laps` | laps of fuel at that rate |
| `fuel.state` | `ok`, `short` (will not reach the flag), `critical` (under a lap), `empty`, `unknown` |
| `ers.present` | the car has a hybrid |
| `ers.charge_pct` | battery charge, % |
| `ers.lap_pct` | deployment budget left this lap, % (*null* without a budget) |
| `ers.mode` | the deployment mode: `BAL`, `ATK`, `HARV` |
| `ers.deploying`, `ers.harvesting`, `ers.boost` | the motor drives / recovers / the overtake button is held |
| `ers.stint_pct` | the stint's energy budget left, % (*null* without a stint rule) |
| `engine.water_c` | coolant temperature |
| `engine.water_state` | `ok`, `hot` (over 105), `over` (over 112, losing power), `unknown` |

### Tyres, brakes and damage

| Name | Meaning |
|---|---|
| `tyre.known` | the server sends tyre temperatures |
| `tyre.compound` | the compound's letter: `S`, `M`, `H`, `I`, `W`, or the first letter of the car's own compound name (*null* when unknown) |
| `tyre.age_laps` | laps the set has done: since the start, or since the last stop the HUD saw (*null* when the compound is unknown) |
| `tyre.wear_max_pct` | the most worn of the four, % |
| `tyre.optimal_c`, `tyre.window_c` | the car's working window: optimal ± window |
| `tyre.<fl|fr|rl|rr>.<field>` | each tyre's figures by name, with the fields of the `tyres` list: `tyre.fl.temp_c`, `tyre.rr.wear_pct`, ... |
| `damage.known` | the server sends damage |
| `damage.level` | the session's damage rule: `off`, `reduced`, `full` |
| `damage.front`, `damage.rear`, `damage.left`, `damage.right`, `damage.engine` | each zone, %; 100 puts the car out |
| `damage.front_flash`, `damage.rear_flash`, `damage.left_flash`, `damage.right_flash`, `damage.engine_flash` | 1 at a fresh hit, fading to 0 over 0.8 s |

### The pit stop

When a car stops at its box the server sends the crew's plan once
(`PitService`: tyres, then fuel, then repairs, each its seconds; a part of
0 s is not done). These say where the stop stands, read against
`pit.service_s`. All are *null* unless the local car is being serviced and the
plan has arrived (a recorded race has none). The shipped `pit_stop` component
draws them. See [pit lane](../server/pit-lane.md).

| Name | Meaning |
|---|---|
| `pit.service_total_s` | the whole stop |
| `pit.service_elapsed_s` | seconds since the crew started |
| `pit.service_progress` | elapsed over total, 0 to 1 |
| `pit.service_phase` | the part in hand: `tyres`, `fuel` or `repair` |
| `pit.service_phase_label` | in words: "CHANGING TYRES · SOFT", "REFUELLING · +25.0 L", "REPAIRING · 50%" |
| `pit.service_phase_left_s` | seconds left of the part in hand |
| `pit.service_phase_progress` | how much of the part in hand is done, 0 to 1 |
| `pit.service_tyres_s`, `pit.service_fuel_s`, `pit.service_repair_s` | each part's seconds, 0 when not done |
| `pit.service_tyres_share`, `pit.service_fuel_share`, `pit.service_repair_share` | each part's share of the stop: a `fill` for a bar's segments |
| `pit.service_tyres_fill`, `pit.service_fuel_fill`, `pit.service_repair_fill` | how much of each part is done (0 until it starts, 1 once over) |
| `pit.service_fuel_l` | the litres going in |
| `pit.service_repair_pct` | the damage being repaired, percent summed over the zones |
| `pit.service_compound` | the set going on, `SOFT`, `MEDIUM`, `HARD`... (*null* when the tyres stay on) |

### Lists

Read with a `repeat`; each row's fields are `item.<field>`.

**`standings`**: every car, leader first (finishers in classified order, then
the furthest round).

| Field | Meaning |
|---|---|
| `item.position` | place, from 1 |
| `item.car_index` | car index |
| `item.name` | driver name |
| `item.car_name` | the car's model (*null* for a car this machine does not have) |
| `item.is_local` | the car the HUD is about: the player's, or the watched car |
| `item.is_player` | the player's own car (never while watching) |
| `item.finished`, `item.finish_position` | has taken the flag, and where |
| `item.retired` | out of the race |
| `item.lap` | lap, as `lap.display` |
| `item.gap_leader_s` | seconds behind the leader (*null* for the leader, or when the length is unknown) |
| `item.interval_s` | seconds behind the car one place ahead (*null* as above) |
| `item.laps_down` | whole laps behind the leader |
| `item.gap_s` | seconds behind the local car (negative: ahead) |
| `item.best_lap_s`, `item.last_lap_s` | lap times |
| `item.last_lap_invalid` | the last lap was struck |
| `item.is_session_best` | the car holds the session's fastest lap |
| `item.speed_kph` | speed |
| `item.in_pit`, `item.on_track` | |
| `item.servicing` | stopped at its box |
| `item.compound` | the compound letter (*null* when unknown) |
| `item.tyre_age_laps` | laps on that set |
| `item.tyre_wear_pct` | its most worn tyre, % |
| `item.pit_stops` | stops made since the HUD first saw it |

**`sectors`**: the local car's lap in progress (just past the line, the lap it
finished).

| Field | Meaning |
|---|---|
| `item.number` | from 1 |
| `item.time_s` | the split (*null* until driven) |
| `item.best_s` | the driver's best for it |
| `item.session_best_s` | the session's best for it |
| `item.state` | `session_best`, `personal_best`, `slower`, `none` |
| `item.is_current` | the car is in it |

**`tyres`**: FL, FR, RL, RR.

| Field | Meaning |
|---|---|
| `item.key`, `item.name` | `fl` / `FL` ... |
| `item.temp_c` | tread temperature (the mean of three zones) |
| `item.pressure_kpa` | running pressure |
| `item.wear_pct` | wear, %; grip falls away past 70 |
| `item.brake_c` | the brake on that corner |
| `item.state` | the tread against the window: `cold`, `ok`, `hot`, `over` (15 °C past it), `unknown` |
| `item.brake_state` | `cold` (under 150), `ok`, `hot` (850+), `over` (1000+), `unknown` |
| `item.inner_c`, `item.outer_c` | the tread's shoulders; the middle is `3 * temp_c - inner_c - outer_c` |
| `item.brake_wear_pct` | pad and disc wear on that corner, % |
| `item.sliding` | sliding hard enough to smoke |
| `item.locked` | the wheel is locked under braking |

**`damage`**: front, rear, left, right, engine.

| Field | Meaning |
|---|---|
| `item.key`, `item.name` | `front` / `Front` ... |
| `item.pct` | damage, % |
| `item.flash` | as `damage.<zone>_flash` |

## The HUD editor and layouts

Settings > Gameplay > HUD layout > Edit layout, or `apexsim.hud.Edit`
(`UI/ApexHudEditorWidget`). It lays the HUD out over the race, or outside a
race over `FApexHudPreview`, a made-up race fed through the same `Build`.
Settings is hidden, not closed, while it runs.

| | Mouse | Keyboard | Pad |
|---|---|---|---|
| Pick a panel | click it | Tab / Shift+Tab | shoulders |
| Move it | drag it | arrows (Shift: 10 at a time) | left stick, D-pad |
| Resize it | drag its corner handle | `[` `]` | triggers |
| Remove it | the **-** on its corner | Del | Y |
| Add a removed one | **+ Add panel** | Ins | A |
| Put it back where it shipped | | R | X |
| Hide the toolbar | Hide | H | Back |
| Layout manager | Layout | L | |
| Save / cancel | Save / Cancel | Enter / Esc | Start / B |

A dragged panel snaps to the gutters, centre lines and other panels' edges
(`ApexHudPlace::Snap`; Shift places freely). Sizes run 50% to 200%. Moving
one panel first **pins** every panel where it is (`UApexHudWidget::PinAll`),
each to the nearest of nine anchor points (the screen in thirds), so the rest
of its region does not close up.

**`custom/layout.json`** (in the first HUD directory) is what Save writes:

```jsonc
{
  "version": 1,
  "components": {
    "car_state": { "enabled": true, "anchor": [1, 1], "position": [-56, -30] },
    "standings": { "enabled": true, "anchor": [0, 1], "position": [828, -310], "scale": 1.25 },
    "minimap":   { "enabled": false }
  }
}
```

`enabled` (no entry: the component's own `default_enabled`), `anchor` (the
panel's corner and the screen point it is measured from, 0 to 1 each axis),
`position` (offset in 1080p pixels, negative left/up), `scale` (0.5 to 2). An
entry without `anchor` and `position` keeps its region. Delete the file to go
back to the shipped layout.

**Several layouts** (`ApexHudLayouts`): `layout.json` is the layout
"Default"; others are `custom/layouts/<name>.json`. The layout manager
switches, saves as new, renames, deletes and binds them; bindings live in
`custom/layout_bindings.json`:

```jsonc
{ "version": 1, "everywhere": "Plain", "watching": "Stream", "hotlap": "Clean",
  "classes": { "Formula": "Wing" } }   // by the class names the menus show
```

Most specific wins: watching, then hotlap and qualifying, then the car's
class, then everywhere else (Default when unset).
`UApexHudWidget::RefreshContext` swaps the layout when the place changes
(from `spectate.active`, `session.mode` and the pending car's class).

## Adding a data point

- The data is `FApexHudData`, built every frame by `ApexHudData::Build` from
  `FApexHudInputs`, which `UApexHudDataSubsystem` gathers from the net, flow
  and settings subsystems. `Build` is pure, so tests feed it a synthetic
  frame. State that spans frames (the delta's reference lap, fuel per lap,
  damage flashes, the rev scale, tyre age, pit stop counts) is
  `FApexHudMemory`.
- A new data point is one `Out.Set(...)` in `ApexHudData.cpp`, **set on every
  path** (`SetNone` where it cannot be known), plus a line in this file. A
  new list field goes in `ApexHudData::ListFields()`.
- Prefer a new data point over pushing logic into expressions: stateful work
  belongs in `Build`. (A scripting language was considered and not used: it
  would cost a VM, a sandbox and a call per value; a script host could later
  publish into the same registry.)
- `UApexHudDataSubsystem` exposes `GetNumber` / `GetText` / `GetBool` /
  `HasValue` to Blueprints and other code.

## Checking it

- `ApexSim.Hud.Data.Stable`: every name exists in every game state.
- `ApexSim.Hud.Data.Documented`: every data point, list field and function is
  in this file (per-tyre names as the `tyre.<fl|fr|rl|rr>.<field>` pattern).
- `ApexSim.Hud.Shipped`: the shipped components load with no errors and no
  warnings (a warning is usually a misspelt data point).
- `ApexSim.Hud.Data.Build`, `.Sky`, `.Watching`, `.StandingsHold`,
  `ApexSim.Hud.Expr.*`, `ApexSim.Hud.Component.*`, `ApexSim.Hud.Json.Extras`,
  `ApexSim.Hud.Layout.*` (`Json`, `Pin`, `Snap`, `Bindings`),
  `ApexSim.Hud.Pit.Progress`.
- Console: `apexsim.hud.Reload`, `apexsim.hud.Data [filter]`,
  `apexsim.hud.Edit`, `apexsim.hud.EditStep <step>`.
- Unattended editor runs: `-ApexOpenHudEditor=N
  -ApexHudEditorSteps="select standings;move 500 -350;scale 0.25;save"`.
  Steps: `select <id>`, `move <dx> <dy>`, `scale <d>`, `toggle <id>`,
  `reset`, `layouts`, `layout <name>`, `saveas <name>`,
  `bind everywhere|watching|hotlap|class:<Name>`, `grab <id> [corner]`,
  `dragby <dx> <dy>`, `release`, `save`, `cancel`. `grab` / `dragby` /
  `release` drive the real mouse path with synthesised Slate events.

## Traps

- The host widget keeps one root (`HostRoot`) and swaps its content on a
  rebuild: a user widget never rereads `WidgetTree->RootWidget` once its Slate
  widgets exist, so replacing the root froze the old tree on screen.
- Scripted editor steps run from the world timer, never from a widget's tick:
  a widget ticks inside Slate's paint, when the hit-test grid is half built,
  and a click synthesised there lands on whatever was painted first.
- Every name must be set on every path, or `ApexSim.Hud.Data.Stable` fails and
  a component reading it warns in some states and not others.
- Telemetry fields are appended positionally; a data point from a field an
  older server does not send must come out *null*, not 0.
