# Unreal client

The game client is an Unreal Engine 5.8 C++ project at `game-unreal/`. It
has no physics: the server simulates every car and the client draws what the
telemetry says, builds tracks and cars at runtime from exported content, and
runs the menu shell, HUD, cameras and audio. Build, test and packaging
commands are in [building.md](../building.md).

## Code

`game-unreal/ApexSim.uproject` (`EngineAssociation` 5.8) with five modules under `game-unreal/Source/`:

- `ApexSim` (runtime, game): menu shell (`UI/`), race view (`Race/`), HUD
  (`Hud/`), runtime tracks (`Track/`) and cars (`Cars/`), audio (`Audio/`),
  input config (`Input/`), track guide (`Guide/`), subsystems at the module
  root (`ApexMenuFlowSubsystem`, `ApexSettingsSubsystem`, `ApexBootSettings`,
  `ApexDemoModeSubsystem`, `ApexMultiViewSubsystem`,
  `ApexStartupSplashSubsystem`, ...). Automation tests in `Private/Tests/`.
- `ApexSimNet` (runtime, protocol): the hand-written MessagePack codec,
  `ApexTcpConnection` / `ApexUdpConnection`, `UApexNetSubsystem`, the
  spectator stream reader/writer. Golden-bytes tests in `Private/Tests/`.
- `ApexSimInput` (runtime): DirectInput wheels, pedals and button boxes; see
  [input-and-ffb.md](input-and-ffb.md).
- `ApexSimBoot` (runtime, `LoadingPhase` `PostConfigInit`): the startup
  splash hold, below.
- `ApexTrackEditor` (editor only): the import and bake commandlets
  (`ApexMaterialBake`, `ApexPropImport`, `ApexGroundTexImport`,
  `ApexTrackImport`, `ApexCarImport`, `ApexTrackCatalogSync`); see
  [track-pipeline.md](../content/track-pipeline.md), [props.md](../content/props.md),
  [cars.md](../content/cars.md).

## Networking module (`ApexSimNet`)

- The codec in `ApexProtocolCodec.cpp` matches the server's `rmp_serde`
  output byte for byte. Every wire change is made on both sides together and
  pinned by golden bytes (`Private/Tests/ApexGoldenBlobs.h`,
  `ApexUdpGoldenBlobs.h`, `ApexSpectatorGoldenBlobs.h`) printed by the
  server's `network.rs` tests; the workflow is in
  [protocol.md](../server/protocol.md).
- `ApexTcpConnection` and `ApexUdpConnection` receive on background threads;
  `UApexNetSubsystem` (game instance subsystem) decodes and dispatches on the
  game thread and is what the rest of the game reads (session state, latest
  telemetry, timing board, delegates). A demo or backdrop session is kept out
  of the session delegates ([cameras.md](cameras.md#demo-mode-uapexdemomodesubsystem)).

## Menu shell

The whole shell is built in C++ (`UI/ApexUIStyle.h` for the palette, fonts
and widget factories), so layouts are reviewable as diffs.

- `AApexMenuGameModeBase` creates the root widget and forces
  `PlayerControllerClass` to `AApexPlayerController`.
- `UApexRootWidget` is the frame: background, a `UWidgetSwitcher` indexed by
  `EApexScreen` (`ApexMenuFlowSubsystem.h`), toasts, a back stack, and the
  race-time layers (HUD, pause menu, settings overlay, hotlap garage, track
  guide, HUD editor). Escape and every Back button go through it.
  `ResolveScreenClass` maps a screen to its C++ class; the session browser and
  loading screen still come from their `/Game/UI/Screens/WBP_*` blueprints.
  `EApexScreen` values are appended, never inserted (the switcher is indexed
  by them).
- `UApexMenuFlowSubsystem` holds menu state the protocol has no message for:
  pending car and track, the create screen's choices (persisted on the profile,
  `ApexProfileSave.h`), the server address, catalog lookups and the content
  checksum checks.
- `UApexSettingsSubsystem` owns `UApexSettingsSave` (the binary slot:
  assists, graphics, camera, controls, wheel, audio, car setups). The settings
  overlay (`UApexSettingsWidget`) has seven tabs, `EApexSettingsTab`: 0
  gameplay, 1 assists, 2 graphics, 3 camera, 4 controls, 5 wheel, 6 audio.
- Navigation: screens derive from `UApexScreenWidget` /
  `UApexNavigableWidget` (`UI/ApexNavigation.h`); leaves delegate keys to the
  owning screen. `FApexMenuInputProcessor` is a Slate input preprocessor that
  sees every event, recovers focus and routes the pause key. Menu sounds for
  focus moves come from `ApexNav::FNavigationScope` ([audio.md](audio.md)).

### Main menu (`UI/ApexMainMenuWidget.cpp`)

Two columns. The left "hero" shows what the player was last doing (pending
track and car from the profile) with **Start session** (a one-click start of
the remembered session) and **Change setup** (the create screen). The right
"rail" is a tree of pages:

- **Garage**: Garage (Manage car setups opens the hotlap garage's setup sheet
  over the menu, `UApexRootWidget::OpenSetupEditor`; View car stats opens car
  select), Replays (the replays screen), Tracks (Watch hotlap,
  `StartHotlapWatch`; See track guide opens the track picker). The Garage and
  Tracks pages turn the hero into a list of cars or tracks with a detail panel.
- **Drive**: Create session (Quick = the remembered session; Race weekend,
  greyed "Not yet"; Custom = the create screen), Browse sessions (live
  sessions: watch or join), Connect to server.
- **Watch a race**: the menu's backdrop race full screen (`WatchBackdrop`).
- **Settings**: the settings overlay.

Keys: Tab / shoulders cross columns, Back (pad B) steps up a page, Escape
opens a "Back to main menu / Exit game" overlay, pad Start (or the pause key)
opens settings, Alt+Enter cycles fullscreen / borderless / windowed through
the settings subsystem (it never activates the focused row). Session create,
car select and replays return false from `WantsLiveBackdrop`, so the
backdrop world is hidden behind them (the car turntable shares the world).

## Input mode and focus

Driving bindings are Enhanced Input actions built in C++
(`Input/ApexInputConfig.h`); `AApexPlayerController` owns them and adds the
mapping context only while a race runs. Defaults, wheels and force feedback
are in [input-and-ffb.md](input-and-ffb.md). Three traps live in the shell:

- The menu runs in `FInputModeUIOnly`, where the viewport discards game input
  and no binding fires. The controller switches to `FInputModeGameAndUI` for a
  race (and back, flushing pressed keys, after it).
- Game-and-UI does not move focus. The controller names the game viewport
  widget as the focus target, and `FApexMenuInputProcessor` puts focus back on
  it when an event arrives while driving. Focus left in the shell routes events
  through the focusable root widget, whose default Slate handler eats the left
  stick, D-pad and arrows as navigation: pad steering dies while throttle and
  shoulders still work.
- A Blueprint game mode can serialise its own `PlayerControllerClass` and
  silently win; `AApexMenuGameModeBase` logs an error when the controller is
  not ours (clear the override on `BP_ApexMenuGameMode`).

## Startup settings (`settings.yml`)

The few settings a player may need before the game is usable live in a
plain-text file instead of the save slot: `display` (`resolution`,
`window_mode`, `vsync`, `frame_limit`, `screens`), `server` (`host`, `port`,
`tls`, `tls_verify`, `tls_fingerprint`) and `launcher` (`show`). `UApexBootSettingsSubsystem` (`ApexBootSettings.h`)
reads it at game-instance startup, creates it when absent (seeded from the
desktop's display mode, or from the save slots on an existing install) and
rewrites it when those values change in game.

- Location: next to the executable. `Game/settings.yml` in a release,
  `game-unreal/settings.yml` (gitignored) in the editor. In a package
  `FPaths::ProjectDir()` is `<Release>/Game/ApexSim/`, so the file is in its
  parent.
- The file wins over the save slots for the values it covers.
  `UApexSettingsSubsystem` and `UApexMenuFlowSubsystem` declare it as an
  `InitializeDependency`, adopt its values on load and push back on save.
- Parsing is a hand-rolled flat two-level YAML subset
  (`ApexBootSettingsIo`); an unknown key or bad value is logged and skipped.
  Every write regenerates the whole file, so hand-added comments do not
  survive an in-game change.
- **TLS** (`FApexTlsOptions` in `ApexSimNet/Public/ApexTls.h`, pushed to
  `UApexNetSubsystem::SetTlsOptions` at startup; takes effect on the next
  connect). The TCP connection runs OpenSSL (the engine's, linked into
  `ApexSimNet`; the engine's SSL module only makes contexts in monolithic
  builds) on memory BIOs under the frame layer (`FApexTlsSession`).
  - `server.tls: auto | on | off` (default `auto`). `auto` tries TLS and
    reconnects in plaintext only when the server shows it does not speak it:
    it closes the connection on the hello without sending a byte (what a
    plaintext ApexSim server does), or answers with something that is not a
    TLS record (`ApexTls::ShouldFallBackToPlaintext`). Never after a TLS
    reply, a timeout (10 s) or a refused certificate. `on` never falls
    back; `off` is plaintext.
  - `server.tls_verify: true | false` (default true): check the chain
    against the Windows root store and the host name (IP entries for an
    address, no SNI then). `false` encrypts without checking: development
    only.
  - `server.tls_fingerprint: <SHA-256>` pins the server's certificate
    (DER), replacing the CA check: how a self-signed certificate is trusted.
    Any spelling (colons, spaces, case, `openssl x509 -fingerprint -sha256`'s
    whole line); written back as `AB:CD:...`. A pin also turns `auto` into
    `on`. Changing `host` (connect dialog or launcher) clears it.
  - A refused certificate is not retried (`FApexDisconnectReason::bPermanent`
    -> `Failed`); the message and the log carry the certificate's
    fingerprint and the line to add. The log says which way it connected:
    `Connected to h:p over TLS (TLSv1.3, <cipher>; <trust>; SHA-256 ...;
    server.tls: ...)` or `Connected to h:p in PLAINTEXT`.
    `UApexNetSubsystem::IsConnectionEncrypted()` says the same at runtime.
  - The certificate is checked before `Authenticate` (and its token) is
    sent. UDP is never encrypted; its binding rests on the token that
    `AuthSuccess` delivered over TLS.
  - `launcher.exe` carries the three keys through its own rewrite (it does
    not edit them).
- `launcher.show` belongs to `launcher.exe` (`launcher/main.cpp`): false
  makes the launcher start the game at once without a window;
  `launcher.exe --show` turns it back on. The game only carries it through
  rewrites (`bShowLauncher`).
- `build_release.ps1` ships `Game/settings.sample.yml`, not a live file: a
  shipped `settings.yml` would be adopted over first-run display detection.
  Its text duplicates `ApexBootSettingsIo::Serialise`; keep the two in step.

Tests: `ApexSim.Settings.BootSettings*` (`BootSettingsTls`: the new keys),
`ApexSim.Net.Tls.*` (modes, fingerprint spellings, the fallback decision,
and `Session`: an in-memory handshake against an OpenSSL server with a fresh
self-signed certificate, pinned / wrongly pinned / CA-verified / unverified,
and a plaintext frame answering the hello).

## Triple monitors (`UApexMultiViewSubsystem`, `ApexMultiView.h`, `ApexSideView.h`)

Settings > Graphics > Screens: SINGLE or TRIPLE (`display.screens: 1 | 3`,
`-ApexScreens=3` for one run, `apexsim.view.Screens N`). Two and four are
refused: an even row puts a bezel in front of the driver. On TRIPLE:

- **The window spans** three same-size monitors in a row
  (`ApexMultiView::FindTripleRow`, preferring the row with the primary in the
  middle), borderless. A core ticker compares the window with the span every
  frame and reshapes it, because the engine sizes a borderless window back to
  one monitor on every resolution change. Leaving TRIPLE calls
  `FSceneViewport::ResizeFrame` with the stored resolution, since the window
  was moved behind the engine's back. Without such a row (one monitor) the
  three views share the window, which is how the mode is checked on a single
  screen.
- **Three views through split screen**: two extra local players, the side
  viewers (`AApexSideViewController`, spawned by
  `AApexMenuGameModeBase::SpawnPlayerController` while
  `IsSpawningSideViewer` is set; no input, no HUD). Their
  `AApexSideViewCameraManager::UpdateViewTarget` copies the driver's camera
  cache and turns it by the side angle with that panel's off-axis frustum.
  `UApexGameViewportClient` (set in `DefaultEngine.ini`) lays the columns out
  so player 0 is the middle. Each view is a full render with its own
  auto-exposure, so a seam can differ in brightness when one panel holds the
  sun.
- **The shell is pinned to the centre third** (`SetCentreWidget`); UMG's DPI
  scale follows window height, so menus and HUD keep their one-monitor size.

Geometry is four Graphics sliders on the settings slot
(`TripleScreenWidthCm`, `TripleBezelCm`, `TripleEyeDistanceCm`,
`TripleSideAngleDeg`); the maths is `ApexMultiView::SideView`. The centre
field is `2 atan(W/2 / D)` and fixes the FOV: the camera FOV slider, chase
rung trims and speed boost are ignored on TRIPLE
(`AApexRaceDirector::bFixedFov`). A broadcast or shot camera keeps its own
lens; the side views are laid out for the eye distance that lens implies
(`EyeDistanceForFov`).

Tests: `ApexSim.MultiView.*` (one plane for a flat row, side panels projected
to their edges through the engine's own projection, a long lens keeping the
seam, the monitor row finder).

## Startup splash hold (`ApexSimBoot`, `UApexStartupSplashSubsystem`)

The splash stays up until the menu's backdrop race is running behind it, so
the first thing seen is the finished menu over a moving race, not the window
resizing and the race fading in.

- `ApexSimBoot` is UObject-free and loads at `PostConfigInit`, because the
  engine creates its game window in pre-init. It hooks window creation: when
  the first top-level window is created it opens a copy of the splash
  (`Content/Splash/Splash.bmp`, at the engine splash's rectangle, just
  beneath it), and on that window's `WM_SHOWWINDOW` it cloaks it with
  `DWMWA_CLOAK` (DWM refuses at creation). A cloaked window ticks, renders and
  resizes, but the compositor does not draw it.
- `UApexStartupSplashSubsystem` claims the hold at game-instance init and
  reveals (uncloak, fade the splash out) when `AApexRaceDirector::IsDemoReady`,
  when the track guide is on screen, early when
  `UApexDemoModeSubsystem::IsDemoExpected` is false (demo off, no server and
  no local showcase, refused login, refused create), or after
  `apexsim.splash.MaxSeconds` (20).
- Watchdogs: an unclaimed hold is dropped at engine init complete, a claimed one after 45 s.
- No hold in the editor, commandlets, `-nullrhi`, `-nosplash`,
  `-ApexNoSplashHold`, `-ApexNoDemo`, `-ApexAutoRace`, or exclusive
  fullscreen (a cloaked window cannot own the display).
- Screen grabs through the compositor do not see a cloaked window; a
  screen-grab loop around a launch is how the hold is checked.

## Car motion buffer (`Race/ApexCarMotion.h`)

Cars are puppets of the telemetry. Each frame's sample goes into the car
actor's `ApexMotion::FApexCarMotionBuffer`; every render frame reads the pose
from a playhead that runs `DelayFrames` (2) telemetry frames behind the newest
sample, blended between the samples either side: location lerp, rotation
slerp (the ±180° yaw seam is crossed the short way), steering, speed and revs
blended for the cockpit wheel, dials and engine sound.

- The playhead's clock is in **server ticks**, which are exact where arrival
  times are not. Ticks per second is a least-squares fit of tick against
  arrival time over the last 120 frames (420 assumed until a second has been
  seen; a first measurement more than 10% off re-seats the playhead once),
  trimmed by a critically damped loop (0.4 s error filter, 1.6 s correction,
  at most 8% slew) that holds the delay.
- Past the newest sample the pose is dead-reckoned (position and turn rate)
  for up to 250 ms, then held. A jump of more than 20 m between samples, or a
  tick running back more than a second, restarts the buffer.
- Console: `apexsim.car.InterpDelayFrames`, `apexsim.car.InterpMaxExtrapolationMs`,
  `apexsim.car.InterpDebug 1` (per-car readout of rate, spacing, lag).

Tests: `ApexSim.Motion.*` (steady and lumpy streams, lost frames, a stall,
teleport, measured tick rate, yaw seam).

## Racing line overlay (`Race/ApexRacingLineActor`)

Settings > Assists > Racing line: OFF / BRAKING ONLY / FULL (default off;
the host can forbid it, see [sessions.md](../server/sessions.md)). The server
builds the line per car (`server/src/racing_line.rs`: the track's raceline or
centerline with a quasi-steady-state speed profile) and sends it as
`RacingLine` after `SessionJoined`. `AApexRacingLineActor` draws dots, one
instanced mesh per colour (green flat out, amber at the grip limit or lifting,
red braking; BRAKING ONLY draws just the red), and `SnapToGround` drops them
onto actors tagged `ApexTrackMesh` by line traces once the track is built.
`SetWet` gives them the road's sheen in rain. `-ApexRacingLine=off|braking|full`
overrides the setting for a run.

## Unattended runs and debugging

Screenshots, site shots and in-game checks are made without a keyboard.
Note `-ExecCmds` separates commands with commas, so
`"r.SetRes 1920x1080w;DisableAllScreenMessages"` is one command.

### Timed actions (`UApexRootWidget`)

- `-ApexScreenshotAfter=N[,N...]` grabs the viewport (with UI) at each time;
  works on any screen, not only in a race.
- `-ApexExecAfter="N=command|N=command"` runs console commands N seconds in.
- `-ApexCameraCycleAfter=N[,N]` presses C (camera step).
- `-ApexOpenPause=N`, `-ApexOpenSettings=N` with `-ApexSettingsTab=0..6`.
- `-ApexOpenHudEditor=N` with `-ApexHudEditorSteps="select standings;move 200 -100;save"`.
- `-ApexStartScreen=<EApexScreen index>` (also cvar `apexsim.ui.StartScreen`)
  opens a screen instead of the main menu; `-ApexCreateTab=1` (Conditions),
  `-ApexCreateMode=<EApexGameMode>`, `-ApexCreateLoadResult=N` for the create
  screen; `-ApexGarageTab=N` for the hotlap garage.

### Auto race (`-ApexAutoRace`)

Creates a session and counts it in with no input (never starts the demo):
`-ApexTrack=<stem>`, `-ApexCar=<name>` (else the lobby's first),
`-ApexAiCount=N`, `-ApexLaps=N`, `-ApexCountdown=N`, `-ApexRaceMinutes=N`,
`-ApexMode=<EApexGameMode>` (6 qualifying, 7 race, 8 hotlap), `-ApexNoStart`
(stop in the lobby), `-ApexAiSkill=N`, `-ApexDamage=off|reduced|full`,
`-ApexLockAssists=abs,tc,gearbox,steering,line`, sky
`-ApexWeather=sunny|cloudy|overcast|lightrain|heavyrain`,
`-ApexTimeOfDay=HH:MM`, `-ApexTimeScale=N`, `-ApexChangeable=N`,
`-ApexTrackRubber=N`. In the race: `-ApexView=cockpit|chase|tv|roof|close|near|far`,
`-ApexRacingLine=...`, the shot camera switches ([cameras.md](cameras.md)).
Hotlap: `-ApexHotlapOutAfter=N`, `-ApexHotlapGarageAfter=N`,
`-ApexHotlapReplayAfter=N` (comma lists).

### Watching, replays, guide, content folders

- `-ApexWatch` (the backdrop), `-ApexWatchSession`,
  `-ApexWatchReplay=<file|latest>`, `-ApexWatchHotlap`, with
  `-ApexWatchCamera=`, `-ApexWatchTower=`, `-ApexWatchCar=`,
  `-ApexWatchHideHud`; console `apexsim.watch [...]`. See
  [spectator.md](spectator.md).
- Backdrop: `-ApexNoDemo`, `-ApexNoShowcase`, `-ApexShowcase=<file|id>`,
  `-ApexShowcaseDir=`.
- `-ApexReplay=<file>.apxs` and the `-ApexReplay*` family (record, fps, camera
  and shot options) play a clip with no server; see [spectator.md](spectator.md)
  and [marketing-site.md](../marketing-site.md).
- `-ApexGuide=<Stem>[:Class]`, `-ApexGuideCorner=N`, `-ApexGuideCamera=N`;
  see [track-guide.md](../content/track-guide.md).
- Content folders: `-ApexTracksDir=`, `-ApexCarsDir=`, `-ApexHudDir=`, `-ApexGuideDir=`.

### Clicking through menus (`UI/ApexUiScript.cpp`)

- `apexsim.ui.Texts [filter]` logs every visible text with its centre in
  viewport pixels.
- `apexsim.ui.Click <text> [#N]` clicks the Nth visible exact match
  (case-insensitive, top to bottom, so the title "Create session" is #0 and the
  CREATE SESSION button #1). It releases 50 ms after pressing, because a plain
  `UButton` ignores a press and release in one frame.
- `apexsim.ui.Mouse down|move|up X Y` drives the left button (drags, dials).
- `apexsim.ui.Key <FKey name> [...]` presses and releases keys, pad buttons
  included (`Gamepad_FaceButton_Bottom`, ...), built as Slate builds a pad's,
  so they reach the input processor and focused widget like real input.

They go through Slate's own input path; run them from `-ApexExecAfter`, e.g.
`-ApexNoDemo -ApexStartScreen=3 -ApexCreateMode=7 "-ApexExecAfter=20=apexsim.ui.Click + #3|25=apexsim.ui.Click Qualifying|30=apexsim.ui.Click CREATE SESSION #1"`.

### Console commands by area

- Cameras: `apexsim.cam.Chase|Goto|LookAt|Fov|Release`, `apexsim.tv.View|Shot|Cut|Pace|Debug|DepthOfField`, `apexsim.cockpit.ScreenNits` ([cameras.md](cameras.md)).
- Demo: `apexsim.demo.Enabled|AiCount|Laps|RandomSky|MaxMinutes|Restart`,
  `apexsim.spectate.Info|Next`.
- Cars: `apexsim.car.Rescan`, `apexsim.car.Interp*`, `apexsim.car.DamagePreview "F,R,L,Rt,E"`,
  `apexsim.car.DamageEffects`, `apexsim.car.HeadlightLumens|BrakeLightNits|TailLightNits|WheelMaxDegPerFrame`.
- Tracks and lighting: `apexsim.track.Rescan|KeepLast|BuildBudgetMs|FlatShadows|HorizonShadows`,
  `apexsim.sky.SunStepDeg`, `apexsim.lights.Max|Range`.
- HUD: `apexsim.hud.Reload|Data|Edit|EditStep` ([hud-modding.md](hud-modding.md)).
- Hotlap: `apexsim.hotlap.Out|Garage|Replay|Stop|Tab|Board|Watch`.
- Replays: `apexsim.replay.Save|List`. Guide: `apexsim.guide.*`.
- Display: `apexsim.view.Screens`, `apexsim.splash.MaxSeconds`.
- Input and audio: `apexsim.input.Devices|Rescan`, `apexsim.ffb.Debug`, `apexsim.audio.RenderCars`.
- Net: `apexsim.net.ParseCenterline` (parse the lobby's track centerlines; set from HUD detail).

## Checking it

- Automation tests run headless on the editor build: `Automation RunTests
  ApexSim` (command in [building.md](../building.md)); names as listed above.
- A new `.cpp` must be checked with `-DisableAdaptiveUnity`: anonymous-namespace
  clashes only show once unity builds batch it with its neighbours.

## Traps

- `EApexScreen` and `EApexSettingsTab` index switchers and command-line
  switches: append, do not insert.
- The input-mode, focus and Blueprint game mode traps above; `settings.yml`
  is rewritten wholesale and never shipped live.
