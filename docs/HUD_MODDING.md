# The HUD: components and data points

The race HUD is drawn from files. Every panel on screen is a **component**: a
folder under `content/hud` holding one `component.json`, which says where the
panel sits and what it draws. What it draws comes from **data points**, named
values the game publishes every frame (`car.speed_kph`, `lap.last_s`,
`standings`, ...). The game never draws a panel by name, so a player can move,
restyle, replace or add any of them without touching C++ or the Unreal editor:
edit a file, run `apexsim.hud.Reload` in the console, and the race HUD is
rebuilt in place.

Players who only want to move things do not need any of that: **Settings >
Gameplay > HUD layout** opens the HUD editor, where panels are dragged,
resized, added and removed on screen (see "The HUD editor" below). It writes
the arrangement to `custom/layout.json`, which sits on top of the components'
own placement.

```
content/hud/
  default/            the shipped HUD (one folder per component)
    car_state/component.json
    standings/component.json
    ...
  custom/             the player's own: gitignored, read after default/
    my_relative/component.json
    standings/component.json     <- replaces default/standings
    layout.json                  <- the HUD editor's arrangement
```

In a packaged game the same tree sits in `Hud/` beside `ApexSim.exe`.

## Quick start

1. Copy `content/hud/default/standings` to `content/hud/custom/standings`.
2. Change `"max": 5` to `"max": 10` in the copy.
3. In a race, open the console and run `apexsim.hud.Reload`.

The custom folder replaces the shipped one because it has the same name. A
folder with a new name adds a component. To hide a shipped component, create
`custom/<its name>/component.json` holding `{ "enabled": false }`.

`apexsim.hud.Data` prints every data point with its current value (pass a
filter: `apexsim.hud.Data tyre`). A component that fails to load is left out
and the reason shows in the middle of the screen and in the log; anything
suspicious that does not stop it loading (an unknown key, a data point name
the game does not publish) is a warning in the log (`LogApexSim`).

## Where components are read from

| Source | When |
|---|---|
| `content/hud` in the repo | editor builds (`play_editor.ps1`) |
| `Hud/` beside `ApexSim.exe` | a packaged game (copied by `build_game_standalone.ps1`) |
| `-ApexHudDir=<dir>[+<dir>]` | added on top, last one wins |

Each directory is read as `default/` then `custom/` (or, if it has neither,
as a folder of components). A later folder replaces an earlier one with the
same component name.

## The component file

`component.json` is JSON with two allowances: `//` and `/* */` comments, and
trailing commas.

```jsonc
{
  "name": "Speed",                 // shown in messages
  "description": "Big speedo",     // optional
  "region": "bottom",              // where on screen, see below
  "order": 10,                     // order within the region, lowest first
  "margin": [0, 0, 0, 40],         // space around it, see "Sizes and margins"
  "float": false,                  // true: placed alone at the region's corner
  "visible": "=car.present",       // optional: hide the whole component
  "enabled": true,                 // false: leave it out (how custom/ hides a default)
  "default_enabled": true,         // false: shipped off; the HUD editor offers to add it
  "root": {                        // the element tree
    "type": "panel", "padding": [16, 8], "background": "surface",
    "children": [
      { "type": "text", "font": "display", "size": 64, "text": "{fmt(car.speed)}" }
    ]
  }
}
```

### Regions

The screen has nine regions: `top-left`, `top`, `top-right`, `left`,
`center`, `right`, `bottom-left`, `bottom`, `bottom-right`. Components in the
top and bottom bands sit side by side along the edge (lowest `order` first,
left to right); components in `left`, `center` and `right` stack top to
bottom. Every region keeps 56 px from the sides and 30 px from the top and
bottom of the screen. A `"float": true` component is placed on its own at
its region's corner (offset by its `margin`) and moves nothing else; the
virtual mirror is one.

Keep `center` empty in anything meant for racing: it is the driver's sight
line into the next corner.

The region is where a component starts. Once a player moves it in the HUD
editor, `custom/layout.json` pins it somewhere else, and that wins.

## The HUD editor

**Settings > Gameplay > HUD layout > Edit layout** (or `apexsim.hud.Edit` in
the console) lays the HUD out on screen: over the race when there is one,
otherwise over a made-up race so every panel has something to show. A card in
the middle of the screen lists every component; each is drawn with a frame.

| | Mouse | Keyboard | Pad |
|---|---|---|---|
| Pick a panel | click it, or its name on the card | Tab / Shift+Tab | shoulders |
| Move it | drag it | arrows (Shift: 10 at a time) | left stick, D-pad |
| Resize it | drag its corner handle | `[` `]` | triggers |
| Show / hide it | the card's Show / Hide | Del | Y |
| Put it back where it shipped | the card's Reset | R | X |
| Hide the card | Hide | H | Back |
| Save / cancel | Save / Cancel | Enter / Esc | Start / B |

A dragged panel snaps to the screen's gutters and centre lines and to the
other panels' edges and centres (hold Shift to place it freely). Sizes run
from 50% to 200% in 5% steps. Components shipped with `"default_enabled":
false` (a relative board, a big gear and speed, the conditions) are listed
as hidden: Show adds them. Cancel puts back the layout the editor opened with;
Reset all goes back to the shipped one (until saved).

Moving one panel first *pins* every panel where it is, so the others in its
region stay put rather than closing up the gap. A pinned panel keeps to the
nearest of nine anchor points (the screen cut in thirds; where its centre
falls decides), so one dragged to the bottom right stays 56 from the right
edge and 30 from the bottom on any screen size.

### `layout.json`

The editor writes `custom/layout.json` in the first HUD directory (the repo's
`content/hud/custom/` in the editor, `Hud\custom\` in a packaged game). It is
plain JSON (comments allowed) and can be edited or shared by hand:

```jsonc
{
  "version": 1,
  "components": {
    "car_state": { "enabled": true, "anchor": [1, 1], "position": [-56, -30] },
    "standings": { "enabled": true, "anchor": [0, 1], "position": [828, -310], "scale": 1.25 },
    "minimap":   { "enabled": false },
    "relative":  { "enabled": true }
  }
}
```

| Key | Meaning |
|---|---|
| `enabled` | shown or not; a component with no entry follows its own `default_enabled` |
| `anchor` | the corner of the panel, and the point of the screen, it is measured from: `[0, 0]` top left, `[1, 1]` bottom right, `0.5` the middle |
| `position` | how far the panel's anchor corner sits from the screen's anchor point, in 1080p pixels (negative is left / up) |
| `scale` | size, 0.5 to 2 (1 when left out) |

An entry without `anchor` and `position` keeps the component's own region.
Delete the file to go back to the shipped layout.

## Elements

Every element is an object with a `"type"`. These keys work on all of them:

| Key | Meaning |
|---|---|
| `type` | one of the types below |
| `children` | a list of elements (containers only) |
| `width`, `height` | fixed size in pixels (either may be left out) |
| `margin` | space around the element inside its parent |
| `halign` | `left`, `center`, `right`, `fill`: across a column, or within a panel |
| `valign` | `top`, `center`, `bottom`, `fill`: across a row, or within a panel |
| `fill` | in a row or column, share of the spare room (e.g. `1`) |
| `visible` | `true`, `false` or an expression; a hidden element takes no room |
| `repeat` | draw the element several times, see "Repeating" |

| Type | What it is | Its own keys |
|---|---|---|
| `row` | children side by side | |
| `column` | children top to bottom | |
| `stack` | children on top of each other | |
| `panel` | a filled box around its children | `padding`, `background`, `outline`, `outline_width`, `radius`, `direction` (`column`, `row`, `stack`) |
| `text` | a line of text | `text`, `font` (`body`, `display`, `mono`), `size`, `tracking`, `bold`, `upper`, `color`, `justify` |
| `label` | a caption: `text` in small mono capitals, muted | as `text` |
| `rect` | a filled rectangle (lights, segments, dots) | `color`, `radius`, `outline`, `outline_width` |
| `bar` | a progress bar filled to `value` (0 to 1) | `value`, `color`, `background`, `direction` (`right`, `left`, `up`, `down`) |
| `spacer` | empty room; `"fill": 1` pushes its neighbours apart | |
| `divider` | a hairline rule | `vertical` |
| `keycap` | a key hint chip, `ESC MENU` | `text` (fixed) |
| `image` | a PNG from the component's folder | `file`, `color` (tint) |
| `minimap` | the circuit outline with a dot per car | `color` (the player's dot) |
| `mirror` | the virtual rear-view mirror, while the setting is on | `width`, `height` |

A panel with exactly one child (not a repeat) places that child directly,
using the child's `halign`, `valign` and `margin`. With more than one child,
it lays them out by `direction` (a column unless told otherwise).

The fonts are the menu's: `display` is the condensed heading face, `body` the
running text (the only one with a bold), `mono` the numbers and captions.
`tracking` is letter spacing in thousandths of an em.

### Sizes and margins

`margin` and `padding` take a number (all four sides), `[horizontal,
vertical]`, or `[left, top, right, bottom]`, in pixels at 1080p (the HUD
scales with the screen like the rest of the UI).

### Repeating

`"repeat"` turns one element into several:

- `"repeat": 30` (or `{ "count": 30 }`) draws it 30 times; inside it,
  `index` is 0 to 29. The rev counter's segments are one rect repeated 30
  times, lit when `index < round(car.rpm_fraction * 30)`.
- `"repeat": { "list": "standings", "max": 5, "focus": "is_local" }` draws
  one copy per row of a list (at most `max`; rows past it are left out),
  with `item.<field>` reading that row and `index` its row number from 0.
  `focus` names a true/false field: the window then scrolls to keep the
  first row where it is true in the middle (the player, in the standings).
  Copies with no row to show are hidden.

## Values and expressions

An attribute is either fixed or an **expression** worked out every frame.

- `text`: plain text, with `{expression}` holes:
  `"LAP {lap.display}/{lap.limit}"`. `{{` and `}}` are literal braces.
- `color`, `background`, `outline`: a colour (see "Colours"), fixed.
- `visible`, `bold`: `true` or `false`, fixed.
- `value`: a number, fixed.

Any attribute that starts with `=` is an expression instead:
`"color": "=car.drs_open ? 'live' : 'border'"`, `"value": "=car.throttle"`,
`"text": "=fmt_time(lap.best_s)"`.

### The language

- Literals: `12`, `1.5`, `'text'` or `"text"`, `true`, `false`, `null`.
- Data points by name: `car.speed_kph`. In a repeat, `item.<field>` and `index`.
- Arithmetic `+ - * / %`; `+` joins text when either side is text
  (`'LAP ' + lap.display`). A division by zero is 0.
- Comparison `== != < <= > >=`; logic `&& || !` or `and or not`.
- Choice: `condition ? a : b`.
- Functions, below.

A data point the game cannot fill this frame (an older server, no car yet) is
`null`: it reads as false, as 0 and as empty text, and `has(x)` tells it apart.
Nothing an expression does can change the game; a broken expression is caught
when the component loads.

### Functions

| Function | Result |
|---|---|
| `if(c, a, b)` | `a` when `c` is true, else `b` (same as `c ? a : b`) |
| `switch(x, k1, v1, k2, v2, ..., default)` | the `v` paired with the first `k` equal to `x`; the last odd argument is the default (`null` without one) |
| `has(x)` | whether `x` is not `null` |
| `min(a, b, ...)`, `max(a, b, ...)` | smallest, largest |
| `clamp(x, lo, hi)` | `x` held between `lo` and `hi` |
| `abs(x)`, `floor(x)`, `ceil(x)` | |
| `round(x)`, `round(x, digits)` | rounded to whole numbers, or to `digits` places |
| `fmt(x)`, `fmt(x, digits)` | `x` as text with that many decimals (0 by default); `—` for `null` |
| `fmt_time(s)` | seconds as a lap time, `1:32.104`; `--:--.---` for none |
| `fmt_split(s)` | seconds as a split, `27.431`; `--.---` for none |
| `fmt_gap(s)`, `fmt_gap(s, false)` | a gap, `+0.412` (unsigned with `false`); `—` for none or over 999 s |
| `fmt_delta(s)` | a signed delta, `-0.250`; `—` for `null` |
| `upper(t)`, `lower(t)` | case |
| `str(x)`, `num(x)` | as text, as a number |
| `mix(c1, c2, t)` | colour `c1` blended toward `c2` by `t` (0 to 1) |
| `ramp(x, x0, c0, x1, c1, ...)` | the colour at `x` along the stops, held past either end: `ramp(damage.front, 0, 'text_disabled', 25, 'accent', 60, 'error')` |
| `alpha(c, a)` | colour `c` with opacity `a` (0 to 1) |
| `rgb(r, g, b)`, `rgb(r, g, b, a)` | a colour from 0-255 sRGB channels, opacity 0 to 1 |

### Colours

A colour is a hex code, `#RRGGBB` or `#RRGGBBAA` (sRGB), or one of the
palette's names: `background`, `surface`, `surface_hover`, `border`,
`accent`, `on_accent` (text on the accent), `text` (also `text_primary`),
`text_secondary`, `text_muted`, `text_disabled`, `live` (green), `error`
(red), `focus`, `white`, `black`, `transparent`, and the timing colours
`session_best` (purple), `personal_best` (green) and `tyre_cold` (blue).
Using the names keeps a component in step with the rest of the game's look.

## Data points

Everything below is refreshed every frame while a race view is open. Times
are seconds, temperatures °C, distances metres. "Local" is the car the HUD
is about: the one the player drives or, while watching a race, the car on
screen (every `car.*`, `lap.*`, `tyre.*`, `sector.*`... is then that car's).
Names marked *null* above their meaning can be `null`.

### HUD and session

| Name | Meaning |
|---|---|
| `hud.full` | the HUD detail setting is "All" (the minimap and pedals use it) |
| `hud.time_s` | seconds since the race view opened, for blinking and fades |
| `hud.imperial` | the units setting is imperial |
| `hud.speed_unit` | `KM/H` or `MPH`, by the units setting |
| `hud.gamepad` | the last input came from a pad: key hints should show its buttons |
| `session.track_name` | the circuit's display name |
| `session.car_name` | the local car's display name |
| `session.mode` | `race`, `qualifying`, `practice`, `hotlap`, `countdown`, `lobby`, `sandbox`, `demo_lap`, `replay` |
| `session.mode_name` | the same as shown in the menus ("Race") |
| `session.is_race`, `session.is_hotlap` | shortcuts on the mode |
| `session.conditions` | the sky in words: "Light rain · 21:30 · wind 12 km/h" (*null* outside a session) |
| `session.weather` | "Sunny", "Light rain", ... (*null* outside a session) |
| `session.clock` | the time of day, "21:30" (*null* outside a session) |
| `session.air_temp_c` | the air, when the host set it (*null* when left to the weather) |
| `session.wind_kph` | the wind, when the host set it (*null* when left to the weather) |
| `net.ping_ms` | heartbeat round trip, refreshed every two seconds (*null* before the first) |

### Watching a race

Set while the player watches a race rather than drives in it: the menu's
backdrop race taken full screen (Main menu > Watch a race) or a live session
joined from the browser's Watch button (docs/SPECTATOR.md, "Watching a
race"). The shipped `spectator_tower`, `spectator_driver` and
`spectator_controls` components show only then, and `standings`,
`race_state`, `track_info`, `status` and `mirror` hide.

| Name | Meaning |
|---|---|
| `spectate.active` | watching: there is no car of the player's, the local car is the one on screen |
| `spectate.live` | the race is a live session (not a recorded showcase, file or demo) |
| `spectate.source` | `showcase`, `file`, `demo`, `replay` or `live` (*null* when not watching) |
| `spectate.camera` | the watch camera: `TV`, `CHASE`, `ONBOARD` (*null* when not watching) |
| `spectate.auto` | the TV director chooses the car |
| `spectate.tower_mode` | the timing tower's column, stepped with T: `interval`, `gap`, `last`, `best`, `tyres` |
| `spectate.waiting` | watching, between two races (no cars yet) |

A saved replay (Main menu > Replays) is watched the same way, with
`spectate.source` `replay` and its transport below; the shipped
`spectator_replay` component draws it.

| Name | Meaning |
|---|---|
| `replay.active` | a saved replay is playing |
| `replay.time_s`, `replay.duration_s` | how far into it, and how long it runs (*null* outside a replay) |
| `replay.progress` | `replay.time_s` over `replay.duration_s`, 0 to 1 |
| `replay.rate` | playback speed: 0.25, 0.5, 1, 2 or 4 |
| `replay.paused` | paused (Space) |
| `replay.ended` | at its end, holding the last frame |

### Race order and gaps

| Name | Meaning |
|---|---|
| `race.position` | the local car's place, from 1 (*null* without a car) |
| `race.car_count` | cars in the session |
| `race.leader_lap` | the leader's lap, as `lap.display` (*null* with no cars) |
| `gap.ahead_name`, `gap.behind_name` | the drivers either side (*null* at either end) |
| `gap.ahead_s`, `gap.behind_s` | the gap to them in seconds at the local car's speed (*null* when the circuit's length is unknown) |

### Laps and timing

Lap times, sectors and track limits are the server's: it times every tick.

| Name | Meaning |
|---|---|
| `lap.current` | the local car's lap counter as sent (0 on the grid) |
| `lap.display` | the lap to show ("lap 3"), held at the race distance on the cool-down lap |
| `lap.limit` | race distance in laps, 0 when there is none (hotlap, practice) |
| `lap.laps_left` | laps to the flag (*null* without a race distance) |
| `lap.time_s` | the lap in progress |
| `lap.invalid` | the lap in progress has been struck for leaving the track |
| `lap.last_s`, `lap.last_invalid` | the last completed lap, and whether it was struck |
| `lap.best_s` | the local car's best legal lap this session |
| `timing.delta_s` | seconds up (negative) or down on the reference lap at this point of the lap (*null* until a legal lap is in) |
| `timing.reference_s` | the reference: the quickest legal lap the HUD has watched |
| `timing.session_best_s` | the session's fastest legal lap |
| `timing.session_best_name` | who set it |
| `timing.session_best_is_local` | the local car set it |
| `timing.optimal_s` | the local car's best sectors added up |
| `sector.current` | the sector the car is in, from 1 |
| `sector.count` | sectors in a lap (3 unless the track names its own) |

### The car

| Name | Meaning |
|---|---|
| `car.present` | there is a local car in this session (or a car on screen while watching) |
| `car.index` | its car index |
| `car.driver_name` | its driver's name, as the standings show it |
| `car.pit_stops` | the stops it has made since the HUD first saw it (*null* without a car) |
| `car.retired` | out of the race: a damage zone has reached 100% |
| `car.speed_kph`, `car.speed_mph` | speed |
| `car.speed` | speed in the player's units |
| `car.gear`, `car.gear_text` | gear as a number (-1 reverse, 0 neutral) and as shown (`R`, `N`, `1`...) |
| `car.rpm` | engine speed |
| `car.rpm_max` | the rev counter's scale: the car's rev limiter, or the highest revs seen |
| `car.rpm_fraction` | `car.rpm / car.rpm_max`, 0 to 1 |
| `car.redline_rpm` | the car's redline from its car.toml (*null* when unknown) |
| `car.throttle`, `car.brake` | pedals, 0 to 1 |
| `car.steering` | steering, -1 to 1 |
| `car.in_garage` | parked in the hotlap garage |
| `car.on_track`, `car.colliding` | |
| `car.headlights` | the headlights are on |
| `car.finish_position` | classified place once finished, else 0 |
| `car.drs_allowed`, `car.drs_open` | the DRS flap may open here / is open |
| `car.tow` | share of drag the slipstream saves, 0 to 1 (*null* from an older server) |
| `car.x`, `car.y`, `car.z`, `car.yaw_deg` | position in the track's frame (metres, +X along the start straight, +Y left) and heading |
| `car.station_m` | distance along the lap |
| `pit.in_lane`, `pit.limiter`, `pit.servicing` | in the pit lane / on the limiter / stopped at the box |
| `pit.service_s` | seconds of service left |
| `fuel.liters` | fuel in the tank (*null* from an older server) |
| `fuel.per_lap` | what the last whole lap burnt (*null* until one is measured) |
| `fuel.laps` | laps of fuel at that rate |
| `fuel.state` | `ok`, `short` (will not reach the flag), `critical` (under a lap), `empty`, `unknown` |
| `ers.present` | the car has a hybrid |
| `ers.charge_pct` | battery charge, % |
| `ers.lap_pct` | deployment budget left this lap, % (*null* without a budget) |
| `ers.mode` | the deployment mode's short name (`BAL`, `ATK`, `HARV`) |
| `ers.deploying`, `ers.harvesting`, `ers.boost` | the motor drives / recovers / the overtake button is held |
| `engine.water_c` | coolant temperature |
| `engine.water_state` | `ok`, `hot` (over 105), `over` (over 112, losing power), `unknown` |

### Tyres and brakes

| Name | Meaning |
|---|---|
| `tyre.known` | the server sends tyre temperatures |
| `tyre.compound` | `S`, `M` or `H` (*null* when unknown) |
| `tyre.age_laps` | laps the set on the car has done: since the start, or since the last stop the HUD saw (*null* when the compound is unknown) |
| `tyre.wear_max_pct` | the most worn of the four, % (*null* when the wear is not sent) |
| `tyre.optimal_c`, `tyre.window_c` | the car's working window: optimal ± window |
| `tyre.<fl|fr|rl|rr>.<field>` | each tyre's figures by name, the fields of the `tyres` list below: `tyre.fl.temp_c`, `tyre.rr.wear_pct`, ... |

### Damage

| Name | Meaning |
|---|---|
| `damage.known` | the server sends damage |
| `damage.level` | the driver's damage setting in force: `off`, `reduced`, `full` |
| `damage.front`, `damage.rear`, `damage.left`, `damage.right`, `damage.engine` | each zone, %; 100 puts the car out |
| `damage.front_flash`, `damage.rear_flash`, `damage.left_flash`, `damage.right_flash`, `damage.engine_flash` | 1 at a fresh hit, fading to 0 over 0.8 s |

### Lists

Read with a `repeat` over the list; each row's fields are `item.<field>`.

**`standings`**: every car, leader first (finishers in classified order,
then the furthest round).

| Field | Meaning |
|---|---|
| `item.position` | place, from 1 |
| `item.car_index` | car index |
| `item.name` | driver name |
| `item.car_name` | the car's model, from the catalog (*null* for a car this machine does not have) |
| `item.is_local` | the car the HUD is about: the player's, or the watched car |
| `item.is_player` | the player's own car (never while watching) |
| `item.finished`, `item.finish_position` | has taken the flag, and where |
| `item.retired` | out of the race (a damage zone at 100%) |
| `item.lap` | lap, as `lap.display` |
| `item.gap_leader_s` | seconds behind the leader (*null* for the leader, or when the circuit's length is unknown) |
| `item.interval_s` | seconds behind the car one place ahead (*null* for the leader, or when the length is unknown) |
| `item.laps_down` | whole laps behind the leader (0 on the lead lap) |
| `item.gap_s` | seconds behind the local car (negative: ahead) |
| `item.best_lap_s`, `item.last_lap_s` | lap times |
| `item.last_lap_invalid` | the last lap was struck for track limits |
| `item.is_session_best` | the car holds the session's fastest lap |
| `item.speed_kph` | speed |
| `item.in_pit`, `item.on_track` | |
| `item.servicing` | stopped at its box being serviced |
| `item.compound` | `S`, `M` or `H` on the car (*null* when unknown) |
| `item.tyre_age_laps` | laps on that set, as `tyre.age_laps` |
| `item.tyre_wear_pct` | its most worn tyre, % (*null* when not sent) |
| `item.pit_stops` | stops made since the HUD first saw it |

**`sectors`**: the local car's lap in progress (or, just past the line, the
lap it finished).

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
| `item.temp_c` | tread temperature |
| `item.pressure_kpa` | running pressure |
| `item.wear_pct` | wear, %; grip falls away past 70 |
| `item.brake_c` | the brake on that corner |
| `item.state` | the tread against the window: `cold`, `ok`, `hot`, `over` (15 °C past it), `unknown` |
| `item.brake_state` | `cold` (under 150), `ok`, `hot` (850+), `over` (1000+), `unknown` |

**`damage`**: front, rear, left, right, engine.

| Field | Meaning |
|---|---|
| `item.key`, `item.name` | `front` / `Front` ... |
| `item.pct` | damage, % |
| `item.flash` | as `damage.<zone>_flash` |

## For developers

- The data is built by `ApexHudData::Build` (`Hud/ApexHudData.h`) from an
  `FApexHudInputs` that `UApexHudDataSubsystem` gathers from the net, flow
  and settings subsystems. A new data point is one `Out.Set(...)` there, set
  on every path (with `SetNone` where it cannot be known): every name must
  exist whatever the game state (`ApexSim.Hud.Data.Stable`), and must be
  documented in this file (`ApexSim.Hud.Data.Documented`).
- The subsystem is also the way into the data from anywhere else:
  `GetNumber` / `GetText` / `GetBool` / `HasValue` are Blueprint-callable.
- The expression language is `Hud/ApexHudExpression.h`: a compiled tree,
  evaluated once a frame per binding, with no side effects. The host widget
  (`UI/ApexHudWidget`) applies only the values that changed.
- Shipped components must load with no warnings (`ApexSim.Hud.Shipped`).
- The layout is `Hud/ApexHudLayout.h` (the file, and the pin / snap maths,
  pure and tested by `ApexSim.Hud.Layout.*`). The host wraps every component
  in a scale box and puts pinned ones on a canvas over the regions; it keeps
  one root widget for good and swaps the built tree inside it, because a user
  widget never rereads `WidgetTree->RootWidget` once its Slate widgets exist.
- The editor is `UI/ApexHudEditorWidget` (a frame layer under its card does
  the drawing). `apexsim.hud.EditStep select standings | move 200 -100 |
  scale 0.2 | toggle relative | save ...` drives it from the console, and
  `-ApexOpenHudEditor=N -ApexHudEditorSteps="...;..."` from the command line
  for a screenshot run; `grab <id> [corner]`, `dragby <dx> <dy>` and
  `release` drive the real mouse path with synthesised Slate events. Those
  are run from the world's timer, never from a widget's tick: a widget ticks
  inside Slate's paint, when the frame's hit-test grid is only half built, so
  a click synthesised there lands on whatever was painted first. Outside a
  race the data subsystem feeds it `FApexHudPreview`, a made-up race.

### Why files and expressions, not Lua or Blueprints

The game's own way to make UI, UMG widget blueprints, is a poor fit for
modding: a mod would need the Unreal editor at the game's exact engine
version and a cooked `.pak`, and the result is a binary asset nobody can diff.
Everything else a player can add (cars, tracks) is plain files the game reads
at runtime, and so is the HUD.

A scripting language (Lua, as in Assetto Corsa's CSP apps) would buy
arbitrary logic at the price of an embedded VM, a sandbox, and a per-frame
call into it for every value. A HUD needs very little logic once the game
publishes the right numbers: the stateful work (the delta's reference lap,
fuel per lap, standings and gaps, damage flashes) is done by the game and
published as data points, and what is left is layout, formatting and colour
rules, which the expressions cover. If a component ever needs logic they
cannot express, the right step is usually a new data point; a script host
could later publish its own data points into the same registry without
changing the component format.
