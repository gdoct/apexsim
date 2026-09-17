# Roadmap

Priorities:

- **P0**: blocks someone from playing a race start to finish. Fix before anything else.
- **P1**: core sim-racing experience. Players will notice right away when it is missing.
- **P2**: depth and polish that keeps players coming back.
- **P3**: nice to have.

`(new)` marks items that were not on the original list.

## Done: Items from this list that were resolved
- brake light
- reverse gear
- car setup (tyres, engine, transmission, torque, suspension): a Car setup tab, applied on the server per driver
- track limits and lap invalidation: all four wheels off strikes the lap, shown in the HUD
- sector timing: server-side sector lines per track, with splits, bests and deltas
- score board: live standings with gaps, the session's fastest lap, and the results screen
- personal bests / lap records saved: kept on the server per driver, track and car, with the lap's trace for a ghost
- hotlap mode (replaces the demo-lap tile): garage with the car setup, run-up spawn before the line, lap-by-lap timing sheet, ghost car and a replay of the record lap with cycling cameras; multiplayer-capable
| Controller focus lost in game and menus / can't navigate to sections | bug | Pad-only players can't play at all. Earlier fix: `FApexMenuInputProcessor` focus recovery. Check which screens and sections still trap focus. |
| minimap drifting issue | bug | The minimap marker drifts away from the actual car position. |
| Improve car grip | tuning | Do this before tyre wear and tyre types, which both build on the base grip model. |
| Car sim settings (assists) | feature | ABS, traction control, auto or manual gears, and racing line in one place per player. Players expect assists before setups. |
| Manual gearbox with sequential paddles / keys (new) | feature | Shifting exists. Make "manual" a setting so the auto gearbox never overrides the driver. |
| Start/finish bleep | feature | Cheap. Also covers the countdown beeps. |

## P0: playable end to end

| Item | Type | Notes |
|---|---|---|

| Reset / recover to track (new) | feature | Walls now stop cars, so a car can end up stuck against a barrier with no way out. Needs a "return to track" or "back to pits" key with a short time cost. |
| Wheel/pedals tested on real hardware (new) | task | The DirectInput path and wheel force feedback have never run on a real wheel. It's a sim racer, so this is the main input device. |


## P1: core racing experience

| Item | Type | Notes |
|---|---|---|
| Better HUD layout options | feature | Basic toggles, position and scale. |
| Yas marina has green over the track|bug|A green texture overlays the normal track surface.|
| COTA first corner is off | bug | Track data or centerline problem. Players see it on the first lap. |

## P2: depth

| Item | Type | Notes |
|---|---|---|
| Tyre wear | feature | Server already builds up `wear_percent` per tyre from slip, but wear doesn't change grip and isn't shown. Wire it into grip, telemetry and the HUD. |
| Tyre temperature (new) | feature | Usually modelled together with wear, and gives grip a warm-up phase. |
| Multiple tyre types | feature | Needs wear and temperature first, and pit stops to be meaningful. |
| Pit stops: tyres and fuel (new) | feature | Fuel is modelled, and pit lanes are real. Needs a pit box stop, a service menu and a pit speed limiter. |
| Tyre sounds | feature | Server already sends slip per wheel in `DriverFeedback`. |
| Kerb sounds | feature | Server already sends the surface under each wheel in `DriverFeedback`. |
| Flags: yellow, blue, chequered (new) | feature | There is no flag state on the wire yet. Needs server-side incident and lapping detection, plus the HUD and trackside panels (`ApexEmissive_*` tags already exist). |
| Car setup: wings, per-car setups (new) | feature | The Car setup tab has tyres, engine, transmission, torque and suspension as clicks off the file. Still missing: aero (the sim has no wing model), a setup saved per car rather than one for all, and absolute read-outs (the base figures are not on the wire). |
| Replay viewer (new) | feature | Server already writes replays (`replay.rs`). The TV director could play them back. |
| Hotlap polish (new) | feature | The ghost is opaque and tinted (no translucent material at runtime); the replay's trackside camera is placed geometrically, not from the TV director's centerline cameras; there is no leaderboard across drivers in a multiplayer hotlap yet. |
| Better HUD controls | feature | Delta bar, relative box, fuel/tyre widgets as those systems land. |
| Black background after session selection | bug | Should show the selected track in panoramic camera mode. |
| Collision penalties / race start ghosting (new) | feature | Stops a first-corner pileup from ruining online races. |
| Reconnect after disconnect (new) | feature | Rejoin your car in a running session instead of losing the race. |

## P3: nice to have
cd 
| Item | Type | Notes |
|---|---|---|
| Red Bull Ring misses Red Bull statue | bug | The model already exists. Add it to `MANUAL_LANDMARKS` in `osm_layout.py` and re-dress. |
| Weather and time of day (new) | feature | |
| Leaderboards online (new) | feature | Needs personal bests and server persistence. |
| Driver rating / safety rating (new) | feature | |
| H-pattern shifter / soft lock (new) | feature | |
| Championships / race weekends: practice, qualifying, race (new) | feature | Game modes exist on the server. Chain them into one weekend flow in the client. |
| Hotlap mode (new) | feature | Allows players to compete against their best lap times on a track. |
| Ghost driver (new) | feature | Shows a translucent version of the player's best lap on the track. |
| Track specific props | feature | Props that are unique to each track, enhancing visual fidelity and immersion. |

## Suggested order

1. Controller focus, grass penalty, reset to track and reverse: a pad player can finish a race.
2. Hardware wheel test, COTA, brake lights: the first impression is right.
3. Grip tuning, then track limits, sectors, score board and saved personal bests: racing is measurable.
4. Assists and manual gearbox, sounds (start bleep, kerbs, tyres): it feels like a sim.
5. Wear and temperature, pit stops, tyre types: race strategy.
