# Roadmap

The product backlog: bugs, features, content and tooling not yet done.
Simulation and racing-rule gaps (physics, tyres, AI, pit and race rules,
race weekends) are in [simulation-gaps.md](simulation-gaps.md); an item
lives in exactly one of the two.

Items are grouped into **features**: rows that touch the same code, the
same wire change or the same check, so they are built, verified and
committed together. A feature is done when its rows are built and its
check has been run. When something is built, delete its row; when a
feature has no rows left, delete the feature. Git history keeps what was
done.

Priorities (a feature takes the priority of its most urgent row):

- **P0**: blocks someone from playing a race start to finish. Fix before anything else.
- **P1**: core sim-racing experience. Players notice at once when it is missing or wrong.
- **P2**: depth and polish that keeps players coming back.
- **P3**: nice to have.

## P0: playable end to end

None open.

## P1: core racing experience

### Reconnect

All of it lives in the held-seat path (`tick.rs` recipients,
`resolve_recipients`, the resume token, the rejoin banner). Fix the bug and
finish the feature in one pass, with one roster change for the away flag.
Check: kill a client mid-race, restart within the grace, and see the
banner, not the race. Then rejoin, and give the seat up.

| Item | Type | Notes |
|---|---|---|
| Resumed connection is put into the held race | bug | A resumed `Authenticate` holds the seat and sends `RejoinAvailable`, but the tick's telemetry recipients are every non-AI participant (`tick.rs`, `player_recipients`), and `resolve_recipients` sends to any player whose lobby session matches, held seats included. So the resumed client gets the held race's telemetry, its session state follows it, and it enters the race view without the player choosing to rejoin (a game killed by `make_shots.py` puts the next run started within the grace into the killed run's hotlap). Held seats should get no telemetry until `JoinSession` resumes them. |
| Reconnect polish | feature | A dropped client always goes back to the menu and rejoins from its banner; it does not rejoin by itself after a short drop. The banner has no way to give the seat up (creating or joining another session does). Other drivers are not told a car is server-driven (the roster has no away flag), and the away driver does not pit, so a long race can run its tank dry. |

### Results and grid screens

These rows share the results widget and the roster. A grid slot added to
the roster serves the lobby GRID list, and the gap at the flag fixes both
the race result and the backdrop result.

| Item | Type | Notes |
|---|---|---|
| Race results GAP column | bug | `UApexSessionResultsWidget` shows each car's best lap against the fastest, not its gap at the flag, so a race result can read out of order (P2 +9.4, P3 +9.0). |
| Lobby grid view | feature | The session lobby's GRID list is in car-index order, not the starting order the race will use (the roster carries no grid slot); after qualifying only the results screen shows the grid. The create screen's starting order can be rearranged by mouse; keyboard and pad only move your own slot. |
| Backdrop race results | feature | A watched backdrop race shows no results screen: the tower reads FINISHED down the field and the next race starts 6 s later. |

## P2: depth

Suggested order: the track geometry pass comes before circuit dressing (the refresh
order re-exports the scene).

### GPU hang

Do this first in P2. It ends races, and every screenshot or verification
loop below depends on a stable client.

| Item | Type | Notes |
|---|---|---|
| GPU hang in a race | bug | A Silverstone race (`-ApexAutoRace`, 5 AI, TV camera, road state off) stopped with `DXGI_ERROR_DEVICE_HUNG` about 10 s after the track was shown, and a Suzuka race did the same; reruns ran. Seen alongside the VSM "Non-Nanite Marking Job Queue overflow" message ([proposals/nanite-shadows.md](proposals/nanite-shadows.md)); the cause is not known. |

### Spectator stream v2

The format revision is built (the `State` record, the joiner's catch-up,
server replays as `.apxs`, live sessions as streams, the watch controls);
see [game/spectator.md](game/spectator.md). What is left of it:

| Item | Type | Notes |
|---|---|---|
| Stream format | task | `SessionKind::Demo` is kept only for old servers and can go once showcases are everywhere. The promo pipeline records no audio. (Frames stay whole: no delta encoding, see [spectator.md](game/spectator.md).) |
| Streams and replays carry too little | feature | A stream's rows and `State` records leave out the ghost and the driver feedback, so a replay has no tyre or kerb sound (slip, slide and contact surface are in no stream). |

### Hotlap and lap records

Everything here is about a single driver's laps: the ghost, the hotlap
replay, the watch, and the lap-record store that a cross-driver
leaderboard reads. The hotlap watch's seek, ghost and replay use the
stream v2 controls.

| Item | Type | Notes |
|---|---|---|
| Hotlap polish | feature | The ghost is opaque and tinted (`AApexGhostCarActor` tints its own instances); the replay's trackside shots are placed geometrically (`CutReplayShot`), not from the TV director's cameras; a multiplayer hotlap has no leaderboard across drivers (the garage scoreboard is shown in qualifying only). |
| Hotlap watch | feature | The first timed lap is from a standing start 300 m before the line, so it is the slowest. The driver's ladder level (after a crash or a struck lap) is not remembered between watches (a per-track, per-car, per-sky cache would skip the bad laps). Stock setup, tyre and sky only (weather and hour; no wind, air temperature or rubber picks, no saved setup), no pause, seek or speed, no ghost and no replay. The `hotlap_watch` HUD scene is not in the HUD editor, and a corner's number is the detector's, not the circuit's official turn number (the dossier does not hold one). |

### Wet weather on screen

What a wet race looks and reads like on the client. Check all of it in
one screenshot batch: a green start, rain, a drying line, puddles, a
forecast change.

| Item | Type | Notes |
|---|---|---|
| Road state on screen: what is left | feature | The pit lane, curbs and run-off are wet by the lap's one figure, not per bin (their UV `u` is not the lap's station). Puddles have no lateral position on the server, so a puddle covers the cell's width. |
| Road state never checked by hand | task | A rubbered start has been seen (Silverstone, `-ApexTrackRubber=100`, overhead at the line): the band is where the server lays it, about 9% darker. A green start, a drying line after rain, puddles, marbles and debris have not been looked at. The colour and gloss figures in `ApplyRoadStateGraph` are first guesses. |
| Forecast on screen | feature | Nothing shows the weather ahead: no forecast on the create screen, no radar; the HUD names only the next change (`sky.*`). |
| Rain light | feature | Cars carry a `car_rainlight` material slot but nothing drives it. |

### Snow

A server rules change plus a client look. It follows Wet weather on
screen, because it reuses the road-state material and the forecast.

| Item | Type | Notes |
|---|---|---|
| Snow | feature | No snow weather: no grip, slush, ice or melt rules on the server and no snowy look on the client. Worked out in [proposals/roadstate.md](proposals/roadstate.md) (phases 2 and 3). |

### Tyres on screen

The tyre material and the tyre read-outs share per-tyre state from
telemetry. Send it per tyre once, then draw and print it.

| Item | Type | Notes |
|---|---|---|
| Tyre look: what is left | feature | The tyres' water is one figure per car, read under its middle, not per tyre; a tyre carries one flat spot (a second lock-up deepens the first rather than laying another where it happened); the tread is procedural (no sidewall bulge in the normal, no pickup or marbles stuck to the tread, no blistering or heat colour). The look has not been judged on screen against a real wet or a flat-spotted slick. |
| Puncture and wet-tyre read-outs | feature | A puncture shows only as FLAT under the tyre (red under 60 kPa) and in the wear; a slow leak shows only as falling pressure. The garage's stock compound card does not say which wet tyre it fits in the rain. |

### Visible damage detail

One feature in the car parents and the damage actor. The hit point is a
telemetry append.

| Item | Type | Notes |
|---|---|---|
| Visible damage detail | feature | One dent pattern per zone (no hit point on the wire), cosmetic debris the cars drive through, engine smoke always from the tail, no windscreen cracks or hanging parts, the DRS flap and driver never dented, wheels not drawn toed. |

### Track geometry pass

Every row changes what the cars drive on, so all of them go through the
refresh order once and are judged with one AI survey on both backends
(centerline and road mesh) against the commit before. Batching them saves
repeated re-exports, and showcases and guides go stale only once.

| Item | Type | Notes |
|---|---|---|
| Misplaced banking on most circuits | content | `ats-bank` has re-laid only Zandvoort. `ats-bank --all --dry-run` still moves or re-signs spans at Monza, Spa (an invented 18 degree bank), Suzuka, IMS, Hockenheim, Mexico City, Austin and others. Run it, then the rest of the refresh order. |
| Centerline elevation from the DEM | content | Only Spa's centerline `z` has been re-derived (`scripts/dem_elevation.py`); every other circuit drives invented keyframes. `--report` ranks them; wooded circuits need a look at the fit first. |
| Kerb and verge geometry in the road mesh | feature | The road mesh has no crown or bumps; `RoadSurface.pit_lane` is written but never read. The desert and paved ground styles are look-only: the server drives those aprons as grass. |
| Karussell concrete | content | `surface_type: Concrete` is not read by the bake, so the Nordschleife Karussell's bowl drives as asphalt. |

### Circuit dressing

Prop-kit and `ats-dress` work, run after the geometry pass. The kit pieces
and the grandstand family are laid by the same `ApexProps` code that
finishes the two circuits. Check each circuit by hand in the running game.

| Item | Type | Notes |
|---|---|---|
| Mandarina Bay unfinished | content | No corner names (the dossier has one "corner", the circuit itself), the F1 pit building prop (`building_pit_street`) is not placed, no quay rail along the water and no catch fence pass. The circuit has not been seen in the running game. |
| Nordschleife unfinished | content | Numbered marshal posts, the track-over-road bridges (B257 at Breidscheid), rock cuttings, spectator hedges and banks, camp villages, the timing gantry as a crossing, more German signs and the T13 gates, the Hohe Acht tower, vertical graffiti on barriers, skid-mark decals. |
| Stadium grandstand family | content | `bay_10m_stadium_roof`, `bay_10m_stadium_curve6_roof` and `end_cap_stadium` are in the kit, but `ApexProps::LayoutGrandstand` does not lay them (a stand naming them gets `bay_10m_roof` bays); `grandstand/stair_tower` was never built. |
| Kit pieces never placed | content | Kerb markers, sausage kerbs, the podium and the hillside letters exist in the kit but no pass lays them. |
| `sign/hillside_letters` spells a trademark | content | The unplaced GLB reads "RED BULL RING"; it needs a display-name text before any scene uses it. |
| Dressing tooling | task | No `ats-dress --explain` or per-prop source stamp; no `ats-export --strict` that fails on a dropped facet; no aerial-imagery tracing aid for dossiers. |

### Pit garage

The garage modules, the camera in them and their night look. Check all
three at a curved box row (Silverstone) with a car stopped at its box.

| Item | Type | Notes |
|---|---|---|
| Curved pit box row | bug | On a curved box row (Silverstone) the 6 m garage modules overlap on the inside of the bend. |
| Chase camera inside the garage | bug | When a car stops at its box or is towed there, the chase camera ends up in a pillar or the garage wall and the car is hidden. |
| Garage windows at night | content | No emissive garage windows at night. |

### Menus and HUD layouts

Shell screens. All of them go through one pad walk at the end.

| Item | Type | Notes |
|---|---|---|
| Main menu rows not built | feature | Race weekend is greyed "Not yet"; the session browser has no recorded sessions or filters; no replay export; no car stats view (the row opens car select); no direct jump into a track guide (it opens the track picker). |
| Damage rule in the session browser | feature | `SessionSummary` does not carry the session's damage rule, so the browser cannot show it. |
| HUD layouts | feature | No per-track layout bindings, no layout picker outside the HUD editor, and the layout manager's pad navigation has not been checked on a pad: it opens on L only and Reset all has no pad button. The HUD editor is not in the pad walk (its pad map is its own). The several layouts (manager, bindings, switching by place) have never been checked by hand. |
| Wind dial labels | bug | The create screen's wind dial names its directions AHD / AL / L / BL / BHD...: terse, though the caption above it spells the pick out. |

### Controls and rigs

New driving actions share one input-packet change (absolute gear,
handbrake) and one bindings pass (recovery). The hardware rows share one
session with real wheels, pads and screens.

| Item | Type | Notes |
|---|---|---|
| H-pattern shifter | feature | The wire's gear field is filled from a shift delta, not an absolute gear. (The wheel's soft lock is built.) |
| Handbrake | feature | No handbrake action, and nowhere on the wire to send one. |
| Recovery key | feature | Back to track and back to pits are pause-menu rows (and `apexsim.recover`); there is no bindable driving action for them. |
| ERS mode not kept | feature | A run starts in the setup's ERS starting mode; a mode switched with the key in the car is not kept into the next run, and once the key is pressed the client's mode overrides the starting mode for the rest of the session. |
| Wheelbase profiles for more brands | feature | `ApexWheelProfiles` has no Simagic, Cammus, Asetek or VRS bases; they mix as Generic (no output scaling) until the player picks a profile or gives the peak torque. |
| Wheelbase profiles unchecked on hardware | task | The `ApexWheelProfiles` name rules are written from the product names the drivers are known to report, not read off real devices, so a base can fall through to Generic. The peak torques and the gear / hybrid minimum forces (5% / 3%) have not been felt on any base. |
| Pad rumble under GameInput | task | `ApexFfb::MixGamepad` assumes XInput's motor layout; the GameInput plugin would put the small channels on the trigger motors. |
| Triple screens | feature | No five-wide row (`MaxSplitscreenPlayers` would need 5), no second monitor for telemetry, no shared exposure across the three views. |

## P3: nice to have

### Car content

Car data that the client and importers drop. The brake material comes
with the `[brakes]` tables.

| Item | Type | Notes |
|---|---|---|
| Car content polish | content | The livery `preview` image is parsed onto the row but drawn nowhere; the GT3 and LMP2 class wheels lack the sidewall lettering; no shipped car.toml sets the tyre pressure keys or lists its own `[[tires.compound]]` / `[brakes]` tables (all run on defaults). |
| HUD brake colours | feature | The HUD colours brakes by temperature alone; the client does not know carbon from steel. |
| AC imports shed nothing | feature | The car importer writes no `[[damage_part]]` tables, so AC imports dent and smoke but shed nothing. |

### Hosting and online

For people running a server for others.

| Item | Type | Notes |
|---|---|---|
| Server dashboard | feature | Not built: a schedule view, start / stop / restart, a broadcast message, content upload and toggles, ping per player, fastest lap and laps in History, kicking an AI car, live config. |
| Driver rating / safety rating | feature | |
| Shared leaderboards | feature | Lap records are per server; nothing is shared between servers. |
| UDP is unencrypted | feature | Telemetry and player input travel in the clear (no DTLS). Every inbound datagram is sealed (HMAC under a per-connection key from `AuthSuccess`, rising sequence number), so a forged, tampered or replayed input is dropped; what is left open is only reading telemetry and input off the wire, which carries nothing the other players do not already get. TCP has TLS. |

### Rendering and platforms

| Item | Type | Notes |
|---|---|---|
| Windows on Arm (Snapdragon X) | feature | The release is x64 only. On an Asus Zenbook A14 (Snapdragon X, Adreno GPU, x64 emulation) DirectX 12 showed only the sky; DirectX 11 (`display.graphics_api: dx11`) drew the scene at about one frame a second, then hung. Not investigated: no log came off the machine. |
| Integrated GPUs | task | The LOW preset (Lumen, virtual shadow maps and distance-field shadows off) and the integrated-GPU first-run default are seen only on an RTX 3080 at LOW. No integrated GPU (Intel Iris Xe / Arc, AMD Radeon 680M / 780M) has run the game yet, so the frame rate there and whether `GRHIDeviceIsIntegrated` picks LOW are unknown. Until one has, the requirements say a discrete GPU is recommended. |
| Mesh distance fields for runtime tracks | task | Runtime-built track meshes have no mesh distance fields, so software Lumen and DF shadows see the road and terrain less well than cooked levels; worth an A/B against an `-ImportLevels` level. |

### Housekeeping

Small, independent cleanups. Do one or two of them alongside whatever
feature touches the same files.

| Item | Type | Notes |
|---|---|---|
| Legacy code | task | `WBP_SessionBrowser`, `WBP_SessionRow` and `UApexSessionRowWidget` are unused since the browser is C++ (`WBP_Root` still references the first); `procgen/` (`--generate-terrain`) and `track_mesh.rs` (used only by `examples/track_export.rs`) look unused; `scripts/content/cars/build_yotota.py` is superseded by `build_lmp2.py`; `L_Menu` still names `/Game/Cars/RB20/SM_RB20` as `DefaultCarMesh`; `ApexRaceDirector.cpp` still matches `MI_wear_*` road bands that no exporter writes. |
| Dead wire fields | task | `SessionKind::Sandbox` has no behaviour and no client sends it. |
| `initialize_content.ps1` and the pit sidecar | task | The script checks the ground, curbs, walls and road sidecars but not `<Stem>.pit.msgpack`. |
| `ats-export --keep-sidecars` help | task | It accepts `pit`, but its help text omits it. |
| Launcher importer offline | task | The launcher's AC import fetches its Python packages from PyPI on first use, so an offline machine cannot import; the launcher's import button has not been clicked by hand. |
