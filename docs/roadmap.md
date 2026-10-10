# Roadmap

The product backlog: bugs, features, content and tooling not yet done.
Simulation and racing-rule gaps (physics, tyres, AI, pit and race rules,
race weekends) are in [simulation-gaps.md](simulation-gaps.md); an item
lives in exactly one of the two. When something here is built, delete its
row; git history keeps what was done.

Priorities:

- **P0**: blocks someone from playing a race start to finish. Fix before anything else.
- **P1**: core sim-racing experience. Players notice at once when it is missing or wrong.
- **P2**: depth and polish that keeps players coming back.
- **P3**: nice to have.

## P0: playable end to end

None open.

## P1: core racing experience

| Item | Type | Notes |
|---|---|---|
| Wheelbase profiles unchecked on hardware | task | The `ApexWheelProfiles` name rules are written from the product names the drivers are known to report, not read off real devices, so a base can fall through to Generic. The peak torques and the gear / hybrid minimum forces (5% / 3%) have not been felt on any base. |
| Race results GAP column | bug | `UApexSessionResultsWidget` shows each car's best lap against the fastest, not its gap at the flag, so a race result can read out of order (P2 +9.4, P3 +9.0). |
| UDP is unencrypted | feature | Telemetry and player input travel in the clear (no DTLS). Since 2026-10-10 every inbound datagram is sealed (HMAC under a per-connection key from `AuthSuccess`, rising sequence number), so a forged, tampered or replayed input is dropped; what is left open is only reading telemetry and input off the wire, which carries nothing the other players do not already get. TCP has TLS since 2026-10-10. |

## P2: depth

| Item | Type | Notes |
|---|---|---|
| Wheelbase profiles for more brands | feature | `ApexWheelProfiles` has no Simagic, Cammus, Asetek or VRS bases; they mix as Generic (no output scaling) until the player picks a profile or gives the peak torque. |
| Reconnect polish | feature | A dropped client always goes back to the menu and rejoins from its banner; it does not rejoin by itself after a short drop. The banner has no way to give the seat up (creating or joining another session does). Other drivers are not told a car is server-driven (the roster has no away flag), and the away driver does not pit, so a long race can run its tank dry. |
| Session lobby shows no track | feature | After joining a session the demo backdrop is left and the lobby has a plain background; it should show the session's circuit (a panoramic or TV camera). |
| Chase camera inside the garage | bug | When a car stops at its box or is towed there, the chase camera ends up in a pillar or the garage wall and the car is hidden. |
| Road state on screen | feature | The client draws neither the rubbered line nor the marbles nor a drying line (the road's sheen is one figure for the lap), and debris (`GameSession::debris`) is not on the wire, so no piece is drawn on the road. |
| Forecast on screen | feature | Nothing shows the weather ahead: no forecast on the create screen, no radar; the HUD names only the next change (`sky.*`). |
| Puncture and wet-tyre read-outs | feature | A puncture shows only as FLAT under the tyre (red under 60 kPa) and in the wear; a slow leak shows only as falling pressure. The garage's stock compound card does not say which wet tyre it fits in the rain. |
| Treaded tyres look like slicks | feature | Intermediates and wets are a tint only (`ApexWheels::TreadedTint`): no tread pattern, so in a wide shot they hardly read apart from a slick. |
| Visible damage detail | feature | One dent pattern per zone (no hit point on the wire), cosmetic debris the cars drive through, engine smoke always from the tail, no windscreen cracks or hanging parts, the DRS flap and driver never dented, wheels not drawn toed. AC imports have no `[[damage_part]]` tables (the car importer writes none): they dent and smoke but shed nothing. |
| ERS mode not kept | feature | The hybrid mode resets to Balanced each race; it is not saved with the player's settings. |
| Hybrid stint budget on the HUD | feature | `ers.stint_pct` is published but no shipped HUD component draws it. |
| Hotlap polish | feature | The ghost is opaque and tinted (`AApexGhostCarActor` tints its own instances); the replay's trackside shots are placed geometrically (`CutReplayShot`), not from the TV director's cameras; a multiplayer hotlap has no leaderboard across drivers (the garage scoreboard is shown in qualifying only). |
| Qualifying results screen and grid view | feature | No results screen after qualifying, and the session lobby's GRID list is in car-index order, not the starting order the race will use (the roster carries no grid slot). The create screen's starting order can be rearranged by mouse; keyboard and pad only move your own slot. |
| Damage rule in the session browser | feature | `SessionSummary` does not carry the session's damage rule, so the browser cannot show it. |
| Live spectating as a stream | feature | A live session is watched through racer telemetry (`SpectatorKind::Live` is reserved): a mid-race joiner gets no earlier lap timing and undercounts pit stops and tyre age. |
| Spectator streams and replays carry too little | feature | `.apxs` streams carry no race clock (a timed race shows no time left), no `PitService` and no live sky (a showcase, a watched replay and the backdrop show the sky the session started under). Replays hold only stream rows: no tyre pressures, brake temperatures, fuel, ghost, or tyre and kerb sound. The server's own replays (`replay.rs`) are not `.apxs`. |
| Replay and watch controls | feature | The timing tower is not clickable; the watch keys are not rebindable; the pad has no replay speed; no scrub bar. Keep and Delete in the Replays screen are keys only (K / Del, pad X / Y): no mouse, and the hint bar names only the keyboard keys. |
| Backdrop race results | feature | A watched backdrop race shows no results screen: the tower reads FINISHED down the field and the next race starts 6 s later. |
| Hotlap watch | feature | The first timed lap is from a standing start 300 m before the line, so it is the slowest. The driver's ladder level (after a crash or a struck lap) is not remembered between watches (a per-track, per-car, per-sky cache would skip the bad laps). Stock setup, tyre and sky only (weather and hour; no wind, air temperature or rubber picks, no saved setup), no pause, seek or speed, no ghost and no replay. The `hotlap_watch` HUD scene is not in the HUD editor, and a corner's number is the detector's, not the circuit's official turn number (the dossier does not hold one). |
| HUD layouts | feature | No per-track layout bindings, no layout picker outside the HUD editor, and the layout manager's pad navigation has not been checked on a pad: it opens on L only and Reset all has no pad button. The HUD editor is not in the pad walk (its pad map is its own). |
| Misplaced banking on most circuits | content | `ats-bank` has re-laid only Zandvoort. `ats-bank --all --dry-run` still moves or re-signs spans at Monza, Spa (an invented 18 degree bank), Suzuka, IMS, Hockenheim, Mexico City, Austin and others. Run it, then the rest of the refresh order and the AI survey. |
| Centerline elevation from the DEM | content | Only Spa's centerline `z` has been re-derived (`scripts/dem_elevation.py`); every other circuit drives invented keyframes. `--report` ranks them; wooded circuits need a look at the fit first. |
| Mandarina Bay unfinished | content | No corner names (the dossier has one "corner", the circuit itself), the F1 pit building prop (`building_pit_street`) is not placed, no quay rail along the water and no catch fence pass. |
| Curved pit box row | bug | On a curved box row (Silverstone) the 6 m garage modules overlap on the inside of the bend. |
| Nordschleife unfinished | content | The Karussell's concrete bowl (`surface_type: Concrete` is not read by the bake), numbered marshal posts, the track-over-road bridges (B257 at Breidscheid), rock cuttings, spectator hedges and banks, camp villages, the timing gantry as a crossing, more German signs and the T13 gates, the Hohe Acht tower, vertical graffiti on barriers, skid-mark decals. |
| Main menu rows not built | feature | Race weekend is greyed "Not yet"; the session browser has no recorded sessions or filters; no replay export; no car stats view (the row opens car select); no direct jump into a track guide (it opens the track picker). |
| Rain light | feature | Cars carry a `car_rainlight` material slot but nothing drives it. |
| Stadium grandstand family | content | `bay_10m_stadium_roof`, `bay_10m_stadium_curve6_roof` and `end_cap_stadium` are in the kit, but `ApexProps::LayoutGrandstand` does not lay them (a stand naming them gets `bay_10m_roof` bays); `grandstand/stair_tower` was never built. |
| Kerb and verge geometry in the road mesh | feature | The road mesh has no crown or bumps; `RoadSurface.pit_lane` is written but never read. The desert and paved ground styles are look-only: the server drives those aprons as grass. |

## P3: nice to have

| Item | Type | Notes |
|---|---|---|
| Leaderboards | feature | The server keeps every driver's best lap per track and car (`records/lap_records.json`), but a driver is only told their own and the track record (`LapRecord`); no screen lists a track's times, and nothing is shared between servers. |
| Driver rating / safety rating | feature | |
| H-pattern shifter | feature | The wire's gear field is filled from a shift delta, not an absolute gear. (The wheel's soft lock is built.) |
| Wind dial labels | bug | The create screen's wind dial names its directions AHD / AL / L / BL / BHD...: terse, though the caption above it spells the pick out. |
| HUD brake colours | feature | The HUD colours brakes by temperature alone; the client does not know carbon from steel. |
| Triple screens | feature | No five-wide row (`MaxSplitscreenPlayers` would need 5), no second monitor for telemetry, no shared exposure across the three views. |
| Mesh distance fields for runtime tracks | task | Runtime-built track meshes have no mesh distance fields, so software Lumen and DF shadows see the road and terrain less well than cooked levels; worth an A/B against an `-ImportLevels` level. |
| Server dashboard | feature | Not built: a schedule view, start / stop / restart, a broadcast message, content upload and toggles, ping per player, fastest lap and laps in History, kicking an AI car, live config. |
| Launcher importer offline | task | The launcher's AC import fetches its Python packages from PyPI on first use, so an offline machine cannot import; the launcher's import button has not been clicked by hand. |
| `initialize_content.ps1` and the pit sidecar | task | The script checks the ground, curbs, walls and road sidecars but not `<Stem>.pit.msgpack`. |
| Recovery key | feature | Back to track and back to pits are pause-menu rows (and `apexsim.recover`); there is no bindable driving action for them. |
| Handbrake | feature | No handbrake action, and nowhere on the wire to send one. |
| Pad rumble under GameInput | task | `ApexFfb::MixGamepad` assumes XInput's motor layout; the GameInput plugin would put the small channels on the trigger motors. |
| Dead wire fields | task | `SessionKind::Sandbox` has no behaviour and no client sends it; `GameMode::Replay`'s `tick_replay` is empty. |
| Legacy code | task | `WBP_SessionBrowser`, `WBP_SessionRow` and `UApexSessionRowWidget` are unused since the browser is C++ (`WBP_Root` still references the first); `procgen/` (`--generate-terrain`) and `track_mesh.rs` (used only by `examples/track_export.rs`) look unused; `scripts/content/cars/build_yotota.py` is superseded by `build_lmp2.py`; `L_Menu` still names `/Game/Cars/RB20/SM_RB20` as `DefaultCarMesh`. |
| Spectator stream format | feature | No delta or keyframe encoding of frames; `SessionKind::Demo` is kept only for old servers and can go once showcases are everywhere; the promo pipeline records no audio. |
| Car content polish | content | The livery `preview` image is parsed onto the row but drawn nowhere; the GT3 and LMP2 class wheels lack the sidewall lettering; no shipped car.toml sets the tyre pressure keys or lists its own `[[tires.compound]]` / `[brakes]` tables (all run on defaults). |
| Track pipeline tooling | task | No `ats-dress --explain` or per-prop source stamp; no `ats-export --strict` that fails on a dropped facet; no aerial-imagery tracing aid for dossiers. `ats-export --keep-sidecars` accepts `pit` but its help text omits it. `ApexRaceDirector.cpp` still matches `MI_wear_*` road bands that no exporter writes. |
| Kit pieces never placed | content | Kerb markers, sausage kerbs, the podium and the hillside letters exist in the kit but no pass lays them; no emissive garage windows at night. |
| `sign/hillside_letters` spells a trademark | content | The unplaced GLB reads "RED BULL RING"; it needs a display-name text before any scene uses it. |

## Built but never checked by hand

- The HUD's several layouts (manager, bindings, switching by place).
- Mandarina Bay in the running game.
