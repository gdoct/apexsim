# Cameras and demo mode

Every camera in a race belongs to the race director (`AApexRaceDirector`,
`Race/ApexRaceDirector.h`): the cockpit rig, the chase ladder, the free
screenshot camera, the TV director and the lobby camera. The menu's backdrop
race (demo mode) is filmed by the TV director; a session's lobby shows its
circuit through the lobby camera. Spectator controls, replays and showcases are
in [spectator.md](spectator.md); triple-screen side views in
[client.md](client.md#triple-monitors-uapexmultiviewsubsystem-apexmultiviewh-apexsideviewh).

## Code

- `game-unreal/Source/ApexSim/Public/Race/`: `ApexCockpitRig.h`,
  `ApexCockpitLayout.h`, `ApexChaseView.h`, `ApexShotCamera.h`,
  `ApexRaceCoordinate.h`, `ApexTvDirector.h`, `ApexLobbyCamera.h`, `ApexFinishCamera.h`,
  `ApexPitCamera.h`, `ApexMarshals.h`, `ApexRaceDirector.h`
- `UI/ApexCockpitDashWidget.h`, `UI/ApexMirrorWidget.h`
- `ApexDemoModeSubsystem.h` (module root)
- Tests: `Private/Tests/CockpitLayoutTests.cpp`, `ChaseViewTests.cpp`,
  `ShotCameraTests.cpp`, `TvDirectorTests.cpp`, `LobbyCameraTests.cpp`, `FinishCameraTests.cpp`,
  `PitCameraTests.cpp`, `MarshalsTests.cpp`

## Camera modes

The director's `ApplyCameraMode` decides which camera is active; car spawns,
a change of followed car, C (`CycleView`) and the settings all go through it.
The view a race opens in is the Camera tab's "Start in" (cockpit or chase,
`UApexSettingsSave::bStartInCockpit`) and the stored chase rung, overridden
for a run by `-ApexView=cockpit|chase|tv|roof|close|near|far`. Camera
settings are applied live through `AApexRaceDirector::ApplyCameraSettings`.

## Cockpit view (`AApexCockpitRig`, `ApexCockpit::DeriveLayout`)

The car meshes are exteriors, so the cockpit is built at runtime. The
director spawns `AApexCockpitRig` and attaches it to the followed car:

- **Steering wheel** rolling with the steering telemetry: the car's own
  `steering_wheel_model` (car.toml), else its class's
  `apexsim-content/wheels/steering/<model>.glb` (built by
  `apexsim-content/tools/generators/wheels/build_steering_wheels.py`), else a flat-bottomed rim
  of engine basic shapes. While a wheelbase steers the player's own car the rig
  turns the rim exactly as far as the player's
  (`SetDriverRimLockDeg`, fed by `AApexRaceDirector::UpdateSteeringLock`; see
  [input-and-ffb.md](input-and-ffb.md)).
- **Dash** (`UApexCockpitDashWidget`): gear, speed, rev lights, lap time, the
  countdown on the grid, race control's phase in place of the lap (`SC`,
  `VSC`, `FORM`) with the pace it holds the car to after it (`SC 80`, in
  the driver's units: `DriverFeedback.speed_cap_mps`, the player's own
  car only). While a yellow or the blue is out for the car
  (`race_flags`, `SetFlags`) the whole light bar flashes its colour at 2 Hz,
  yellow over blue, a double yellow at 4 Hz, as a real wheel's does. On a class wheel it sits on the wheel's screen, placed
  by `<model>.json` beside the GLB.
- **Mirrors**: `USceneCaptureComponent2D`s into render targets shown on
  `UApexMirrorWidget` faces, drawn flipped. The rig refreshes them
  round-robin, never every capture every frame; the Mirror quality setting
  picks resolution, how many refresh per frame and whether they draw dynamic
  shadows. A fourth capture feeds the HUD's virtual mirror strip.
- The screens are unlit widget components; at race exposure they would be
  black at a tint of 1, so `apexsim.cockpit.ScreenNits` (default 4500) scales
  them. That is their brightness in a view exposed at
  `apexsim.cockpit.ScreenReferenceEv` (EV100 14, a sunny race) or above; a
  view exposed lower (dusk, night) dims them by as many stops
  (`ApexCockpit::ScreenNitsAt`), so the display and mirrors read the same
  on screen at night as by day instead of as white rectangles. The
  exposure is the game view's own eye adaptation, read as each frame's
  views are set up (`FApexCockpitExposureProbe`, a world scene view
  extension that skips scene captures).

Placement is `ApexCockpit::DeriveLayout`: pure maths over the mesh's bounds
in the car frame, open cockpit or closed cabin from the catalog class (else
from height). Per-car overrides are `FApexCarCatalogRow::Cockpit` (eye,
wheel, mirrors; zero means derive), filled from car.toml `[cockpit]` (see
[cars.md](../../../apexsim-content/docs/cars.md)). The player's seat slide and height, view
pitch, horizon lock, head motion (inertia from speed and yaw-rate estimates)
and look-to-apex are on `UApexSettingsSave`'s camera block.

## Chase ladder (`ApexChase::Views()`, `Race/ApexChaseView.h`)

C steps from the cockpit down a ladder of chase distances and back to the
cockpit. Rungs, closest first: `roof` (boom 0.6 m back, lifted 1.4 m, near
the driver's eyeline), `close` (3 m), `near` (5.6 m), `far` (9 m). Each rung
carries arm length, lift, boom pitch, a field-of-view delta on top of the
chase FOV, the spring arm's lag speeds and a lag clamp
(`CameraLagMaxDistance`).

- `AApexRaceDirector::ApplyChaseView` pushes a rung onto the boom;
  `UpdateLook` takes the pitch from it.
- The rung is stored in `UApexSettingsSave::ChaseViewLevel` (default 3, far)
  with a "Chase distance" row on the Camera tab. The row only stores it (the
  director adopts it with the rest of the camera group, so picking a distance
  while in the cockpit does not leave the cockpit); C writes back through
  `UApexSettingsSubsystem::SetChaseLevel`, so the distance survives the race.
- The hotlap ghost replay borrows the far rung and restores the player's
  afterwards.
- `apexsim.cam.Chase [0-3|roof|close|near|far|cockpit]` (no argument steps).
  `-ApexCameraCycleAfter=N[,N]` with a matching `-ApexScreenshotAfter` list
  walks the whole ladder in one unattended run.

The boom is a `UApexChaseBoom` (`Race/ApexChaseBoom.h`): after the spring
arm has placed the camera, `ApexChase::KeepOutOfBody` lifts it straight up
to 15 cm over the roof whenever it is within 15 cm of the followed car's
body box (`AApexRaceCarActor::GetBodyBox`, handed in by the director each
frame). The lag swings the camera on a sphere round a lagging origin, so a
car that spins or stops dead could otherwise carry its own body through the
shot. Every rung at rest is already clear of a hypercar's box.

Tests: `ApexSim.Camera.ChaseLadder`, `ApexSim.Camera.ChaseCycle`,
`ApexSim.Camera.ChaseKeepOutOfBody`; verify run `b5-chase-chiffon`.

## Pit box pose (`ApexPitCamera`, `AApexRaceDirector::UpdatePitCamera`)

The boom has no collision test, and seen off it a car stopped at its box was
half hidden by a garage column. So in the chase view, while the player's own
car stands at its box (the crew at work, or under 1.5 m/s within 4 m of the
spot; held until it pulls away past 3 m/s), the TV camera takes a pose in the
lane: a metre inside the lane's far edge (the pit wall's, away from the
garages), 4.5 m ahead of the spot, 1.7 m over the car, looking back at it.
The place comes from the server's `PitBox` (the spot's yaw and its side of
the lane's middle). The boom's lag is held off for two frames on the way
back. Not in the cockpit, TV, shot, replay or watch views.

Tests: `ApexSim.Camera.PitBox`.

## Screenshot camera (`ShotCamera`, `Race/ApexShotCamera.h`)

A third camera on the director that parks anywhere on the circuit. While a
pose is set it is the only active camera, the followed car's bodywork is
shown and the cockpit rig hidden, and `ApplyCameraMode` keeps it through
spawns, focus changes and C.

Every number is in the **server frame** (metres, +Y left, yaw
counter-clockwise from +X, pitch positive up), so a shot can be worked out
from the track YAML's centerline. Conversions are in `ApexRaceCoordinate.h`.

- Console: `apexsim.cam.Goto X Y Z [Yaw] [Pitch]`,
  `apexsim.cam.LookAt X Y Z TX TY TZ`, `apexsim.cam.Fov Deg`,
  `apexsim.cam.Release`. Each logs the pose back as an `-ApexCamera=` switch.
- Command line, applied when the race view begins and held for that race:
  `-ApexCamera=X,Y,Z[,Yaw[,Pitch]]`, `-ApexCameraLookAt=X,Y,Z,TX,TY,TZ` (wins
  over `-ApexCamera`), `-ApexCameraFov=Deg` (default 70). Example:
  `-game -ApexAutoRace -ApexTrack=Sudoku -ApexCameraLookAt=... -ApexScreenshotAfter=12`.

Photo mode flies the same camera over a watched race, with a lens of its
own on the camera's post-process (depth of field, exposure); see
[spectator.md](spectator.md#photo-mode).

Tests: `ApexSim.Camera.ParseNumberList`, `ApexSim.Camera.ServerFramePose`.

## TV director (`ApexTv::FDirector`, `Race/ApexTvDirector.h`)

Pure logic with ground and visibility traces injected, so it is tested on a
synthetic ring. In the game (`MakeCameraQueries`) visibility is a complex
trace against world statics both ways, camera to car and back: a camera put
down inside a mesh (a pit wall, a building) sees out of it one way only. A
view blocked for `BlockedCutSeconds` (0.8 s) is cut. A lens inside a mesh
(`FWorldQueries::IsInside`) is never shown at all, not even for that long:
the director cuts again in the same frame, up to `InsideRetries` (3) times,
the last try the chase. A trackside spot inside one is not taken. In the game
a lens is inside when a static face lies within 40 cm of it along any axis,
traced either way (a face beside the line to the car fills a long lens with
a blur), or when traces coming in from 25 m out on all four level sides and
from above all meet a face (a trace meets a face only from its front, so
these see a hoarding's or a building's outside however the lens looks out
of it). It picks a subject (battles, the leader, a car in trouble:
off track or stopped, at most once per 12 s) and cuts between shots
(`EShot`): grid, trackside (a long lens off the road on the outside of the
bend, ahead of the car), helicopter (a low, raking orbit 45-70 m out and
12-22 m up, looking down at 15-25 degrees; the race's opening one frames
the front of the grid), tracking, chase, onboard, nose (`NoseMount`: 5 cm
ahead of the body's front face, low; behind that face the lens is inside the
bumper and a two-sided body fills the frame with its paint) and
reverse, with lag-compensated pans and depth of field. `LockTarget` holds it
on one car (spectating, replays, the track guide).

Trackside and helicopter positions need the circuit's path: in a race the
director feeds it the session track's centerline from the lobby
(`FApexTrackConfigSummary::Centerline`, parsed only while
`apexsim.net.ParseCenterline` is 1); a spectator stream carries its own
`Path` record.

- `-ApexView=tv` or `apexsim.tv.View 1` films a race with it.
- `apexsim.tv.Shot <name|none>` holds one shot, `apexsim.tv.Cut` cuts now,
  `apexsim.tv.Pace` scales hold times, `apexsim.tv.DepthOfField 0`,
  `apexsim.tv.Debug 1` (shot, subject and lens on screen). Each cut, its
  reason (`inside a mesh` for the rule above) and the camera's position are
  logged at `LogApexSim Verbose` (`Broadcast camera:`).

Tests: `ApexSim.Tv.*` (cutting, framing, path, target choice, cars in
trouble, a blocked view, a lens inside a mesh, trackside placement, lock
target).

## Marshals and recovery (`ApexMarshals::FTracker`, `Race/ApexMarshals.h`)

A car stopped off the road or out of the race gets marshals, and a
recovery vehicle that comes for it, drawn by the client from telemetry
alone (nothing on the wire; the tow itself is the server's,
[sessions](../server/sessions.md#virtual-safety-car)). Pure logic,
tested on the synthetic ring; the director draws it
(`ApexRaceDirectorMarshals.cpp`) during a race, live or replayed.

- **When**: slower than `StoppedMps` (1.5 m/s) outside the pits for
  `OffRoadAfterS` (2 s) off the road or with a damage zone at 100%, or for
  `OnRoadAfterS` (6 s) on the road. At most `MaxIncidents` (3) scenes; a new
  one takes the place of a finished one, else the oldest, and a car whose
  scene was dropped or ran its course gets none again until it moves.
- **Marshals**: two kit `misc/marshal_figure` cards side by side at the
  verge `PostUpCm` (12 m) up the road from the car, on its side and at
  least 7 m out (3.5 m beyond a car off the road), facing the oncoming cars,
  each waving a yellow flag (a primitive sheet swinging in the card's plane).
- **Recovery vehicle**: the kit `vehicle/tractor`, with a flashing amber
  roof beacon, sets off `TruckAfterS` (1.5 s) after the marshals came out
  from 45 m up the road and drives in over `TruckDriveS` (5 s) along the
  car's line, 2.5 m further out, to stop 6.5 m behind it: there before the
  server tows a car out of the race (10 s after it went out).
- **Gone**: the car towed to its pit box (it moved 30 m or into the pits) or
  driving on (over 4 m/s): the flags go down, and the scene clears after
  `LingerS` (5 s). A grid or a finished race clears every scene.
- Without the circuit's path the car's own heading and right-hand side
  stand in; without the kit, orange primitives.
- `apexsim.marshals 0` turns them off; `apexsim.marshals.Look` parks the
  shot camera on the newest scene (`apexsim.cam.Release` hands it back).
  Each scene is logged (`Marshals out for car N at (...) cm`).

Tests: `ApexSim.Marshals.*`.

## Finish view (`ApexFinishCam::FMove`)

After the local car takes the flag, or retires from a race with a damage zone
at 100% (`UApexRootWidget::ShowResultsAfterRetirement`: a "Retired" toast;
not under `VisualOnly` damage, where the car races on), the root widget asks
for it (`AApexRaceDirector::BeginFinishView`;
[sessions](../server/sessions.md#race-start-and-finish)) and the director
spectates its own race. The server tows a retired car to its pit box 10 s
later, so the panorama follows it there; the results let the driver pick a car
still racing: the TV camera is flown by
`Race/ApexFinishCamera.h`, a pure move from whichever camera was on screen.
It pulls straight back and up behind the car (3 s, 22 m back, 8 m up,
following the car's eased heading), then climbs into a slow orbit in the
world frame (5 s, to 52 m out and 26 m up, 6°/s), held while the cool-down
driver laps. It keeps 3 m over the ground below and turns from the old
camera's aim onto the car over 0.8 s; a camera more than 100 m away starts
from a chase position behind the car. `FocusFinishCar` puts the broadcast
director on another car (locked) or the move back on ours; `EndFinishView`
gives the driver's own camera back.

Tests: `ApexSim.FinishCam.*` (zoom out behind, rise into the panorama
without a jump, orbit in the world frame, far start, rising ground).

## Demo mode (`UApexDemoModeSubsystem`)

Whenever the player is not in a session, an AI race plays behind the menu on
the director's demo view, filmed by the TV director. Sources, first that can
deliver (`EApexBackdropSource`):

1. the server's showcase channel when the lobby lists one;
2. a local `.apxs` the game rendered itself (`Saved/ApexSim/Showcases/`)
   whose checksums match this machine's content, the pending track's
   first; needs no server;
3. a replay, a live session or a watched hotlap the player chose
   (`PlayReplay`, `WatchLive`, `WatchHotlap`), which holds until they leave it.

The first two are described in [spectator.md](spectator.md). Each plays on
the backdrop feed (`BeginBackdropFeed`: `OnDemoSessionChanged`,
`IsInDemoSession()` true, `IsInSession()` false), and keeps the sky it was
rendered in.

**On screen.** The demo view builds the track (the pending track when it has
an export). The root widget fades page backgrounds by the director's
`GetDemoBackdropOpacity()` under a left-heavy scrim; screens whose
`WantsLiveBackdrop` is false (session create, car select, replays) hide the
demo world, because the turntable shares it. The backdrop moves on after a
finished race, a track change, `apexsim.spectate.Next` or
`apexsim.demo.MaxMinutes` (12).

- Off: `-ApexNoDemo`, `-ApexNoShowcase` or `apexsim.demo.Enabled 0`;
  `-ApexAutoRace` never starts one; `-ApexShowcase=<file|id>`;
  `apexsim.demo.Restart`.
- The startup splash waits on `IsDemoExpected` / `AApexRaceDirector::IsDemoReady`
  ([client.md](client.md#startup-splash-hold-apexsimboot-uapexstartupsplashsubsystem)).

Tests: `ApexSim.Sky.DemoRoll`, `ApexSim.Backdrop.ChooseFile`, `ApexSim.Backdrop.EndsAtJoin`.

## Lobby view (`BeginLobbyView`, `ApexLobbyCam::FFlyover`)

In a session, outside its race (the lobby, the results), the backdrop is the
session's own circuit with nobody on it. `UApexRootWidget::UpdateBackdrop`
begins the director's lobby view while the player is in a (non-demo) session,
the race view is off and the screen `WantsLiveBackdrop`, and ends it once the
backdrop gate has faded behind a screen that does not (car select). The view
builds the session's track (`ResolveTrackStem`, from the track cache when it
was built before), lights it with the session's conditions and fades in by
`GetBackdropOpacity()` under the same scrim as the demo. No track export means
the plain background, as before. `BeginRaceView` takes the view over with the
track already built (`StopLobbyView(bKeepTrack)`); leaving the session or the
demo or a replay beginning ends it.

The camera is pure logic in `Race/ApexLobbyCamera.h`, flying the built
track's own centerline (`UApexTrackInstance::GetCenterline`, with heights;
the lobby's centerline is not parsed unless the minimap wants it). Shots of
7 to 13 s round the tightest of a few random corners, never the same kind
twice running nor the same part of the lap: an establishing **orbit** high
over a corner, a **glide** above the outside of the road flying into a
corner, and a low **pan** on the outside of a bend from entry to exit. A shot
whose view of the road is blocked at its start, middle or end is replanned;
the camera eases over steps in the ground and keeps 2 m above it.
`apexsim.tv.Cut` cuts it; each cut is logged at `LogApexSim Verbose`.

Tests: `ApexSim.LobbyCam.*` (no path, road height, flying the circuit, a
blocked view).

## Traps

- **Stale telemetry after a session switch.** Telemetry carries no session id.
  `SessionJoined` drops every frame already queued from UDP (after a hitch it
  holds seconds of the session just left), and a frame is applied only when
  every car index fits the current roster. Without both, a stale demo frame
  stamps `Countdown`/`Racing` on a session still in Lobby and the race view
  covers the lobby with nothing counting down.
- **Spring-arm lag at speed.** A spring arm's steady-state lag is speed over
  lag speed (nine metres at 320 km/h); without each close rung's lag clamp
  every rung looks like the far one down a straight. The roof rung is all but
  welded on, because lag that close swings the roofline about the frame.
- **Shot camera frame.** `-ApexCamera` numbers are server-frame metres, not
  Unreal centimetres.
- **Unlit cockpit screens** need `ScreenNits`, or they render black at race
  exposure.
