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
  [track-pipeline.md](../../../apexsim-content/docs/track-pipeline.md), [props.md](../../../apexsim-content/docs/props.md),
  [cars.md](../../../apexsim-content/docs/cars.md).
- `ThirdParty/ApexPhysics` (external, no code of its own): the server's car
  step as a prebuilt static library for [client prediction](#client-prediction-raceapexpredictionsubsystemh);
  built by `scripts/build_physics_lib.ps1` ([building.md](../building.md#the-physics-library-serverffi)).

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
- The UDP worker keeps a ring of the last 512 inputs it sent, each with its
  number and when it went out (`FApexUdpConnection::CopySentInputs`,
  `UApexNetSubsystem::CopySentInputs`), and stamps each `DriverFeedback`
  with when it took the datagram in (`ReceivedSeconds`,
  `UApexNetSubsystem::GetOwnCarSnapshotSeconds`): what the prediction times
  its clock by.
- **Artificial latency** (`apexsim.net.FakeLatencyMs N`, `-ApexFakeLatencyMs=N`,
  0 off): a round trip of N ms on the UDP path, so a ping can be felt on
  localhost. The worker holds each input it sends for N/2 ms (sealed when it
  goes, so the seal's sequence still rises in order) and each datagram it
  receives for N/2 ms before decoding it; it wakes in time for the next one
  due. TCP is not delayed, so the heartbeat's ping does not include it.

## Menu shell

The whole shell is built in C++ (`UI/ApexUIStyle.h` for the palette, fonts
and widget factories), so layouts are reviewable as diffs.

- `AApexMenuGameModeBase` creates the root widget (`UApexRootWidget`
  itself: `BP_ApexMenuGameMode` sets no `RootWidgetClass`) and forces
  `PlayerControllerClass` to `AApexPlayerController`. `/Game/UI` holds only
  the track preview textures; there are no widget blueprints.
- `UApexRootWidget` is the frame: background, a `UWidgetSwitcher` indexed by
  `EApexScreen` (`ApexMenuFlowSubsystem.h`), toasts, a back stack, and the
  race-time layers (HUD, pause menu, settings overlay, hotlap garage, track
  guide, HUD editor). Escape and every Back button go through it.
  `ResolveScreenClass` maps every screen to its C++ class (the loading
  screen, `UApexLoadingScreenWidget`, is one centred line).
  `EApexScreen` values are appended, never inserted (the switcher is indexed
  by them).
- Toasts (`UApexToastWidget`) are the top layer of the root, bottom centre,
  hit-test invisible, so they show over the menus and the race HUD without
  taking focus; a toast lasts 4 s. A `ServerNotice` from the operator (a
  dashboard broadcast, or the admin ending the session) arrives as
  `UApexNetSubsystem::OnServerNotice` and shows as `Server: <message>` for
  8 s, in the normal colour with the Notice cue. A later toast replaces it.
- `UApexMenuFlowSubsystem` holds menu state the protocol has no message for:
  pending car and track, the create screen's choices (persisted on the
  driver profile in use, `ApexProfileSave.h`; [below](#driver-profiles-apexprofilesh-uiapexprofilewidgetcpp)),
  the server address, catalog lookups and the content checksum checks.
- `UApexSettingsSubsystem` owns `UApexSettingsSave` (the profile's binary
  slot: assists, graphics, camera, controls, wheel, audio, car setups;
  `settings.yml` still decides the display block). The settings
  overlay (`UApexSettingsWidget`) has seven tabs, `EApexSettingsTab`: 0
  gameplay, 1 assists, 2 graphics, 3 camera, 4 controls, 5 wheel, 6 audio.
- Navigation: screens derive from `UApexScreenWidget` /
  `UApexNavigableWidget` (`UI/ApexNavigation.h`); leaves delegate keys to the
  owning screen. `FApexMenuInputProcessor` is a Slate input preprocessor that
  sees every event, recovers focus and routes the pause key. Menu sounds for
  focus moves come from `ApexNav::FNavigationScope` ([audio.md](audio.md)).
  A surface whose controls sit in a scroll box (create session, settings, the
  garage card) answers directions itself with `ApexNav::MoveToward`: the
  nearest control that way by its current layout (`ApexNav::LayoutRect`,
  arranged afresh, not the last paint), rows of the same scroll box first,
  and a row scrolled out of view by list order. From a row that is itself
  not laid out yet (focused below the fold, still scrolling in) every move
  goes by list order within its scroll box, sideways too: measured, it
  would start from the corner of the screen. `UApexNavigableWidget` sends
  Up/Down on its own slider the same way (`NativeOnPreviewKeyDown`). Every
  screen and overlay is walked by pad in `ApexSim.UI.PadWalk` (below).
- The pause menu in a race: Close menu, Settings, PIT PLAN (a driver in a
  race or free practice), BACK TO GARAGE (hotlap and
  qualifying, on track), BACK TO TRACK / BACK TO PITS (a stuck car; see
  [sessions.md](../server/sessions.md#recovering-a-stuck-car)), Save replay,
  Back to main menu, Exit. On the main menu's root page pad B opens the Back
  to main menu / Exit game menu (Escape opens it from any page).
- PIT PLAN swaps the pause card's rows for the driver's orders for the next
  stop ([pit-lane.md](../server/pit-lane.md)), one stepper each: Tyres (KEEP,
  SETUP'S, then the car's compounds by the setup sheet's names), Fuel (TO THE
  END, NONE, +5 L steps), Repairs, Brake pads (CREW'S CALL, KEEP, NEW), Pad
  set (SETUP'S, ENDURANCE, STANDARD, SPRINT) and Box this lap, with the plan
  in a line and DONE; Back returns to the rows, whose PIT PLAN badge is the
  same line (`ApexPitPlan::Summary`). `UApexPitPlanSubsystem` keeps the plan
  for the session it was given in and sends `SetPitPlan` on every change;
  joining the same session again sends it again (the server forgets it on a
  rejoin), another session starts from the crew's call. The `BoxThisLap` key
  toggles the request ([input-and-ffb.md](input-and-ffb.md)); the subsystem
  drops it once the stop starts, and after a stop on a plan with a chosen
  pad set that fitted pads (always for NEW; for the crew's call when the
  pads' wear fell over the stop) moves the garage setup's `brake_pads` knob
  to that set, as the server moved its own, so the next `SetCarSetup` keeps
  it.
- The create screen (`UI/ApexSessionCreateWidget.cpp`) has a Race tab and
  a Conditions tab, each a scroll box of sections. The Race tab's rule
  sections show for the sessions they apply to: Qualifying, Race weekend,
  and, in a race or a qualifying session, Pit stops and Race control
  (Start Standing / Formation lap / Rolling, Penalise False starts /
  Yellow passes / Pit exit light as toggles, Safety car Off / Virtual / On
  the road, Reverse grid Off / 6 / 8 / 10 / All;
  [sessions.md](../server/sessions.md#session-rules-set-on-create)). Each
  section's caption line and the summary under the screen name what is
  set. Keys and pad reach every chip through `ApexNav::MoveToward`; a new
  row only has to be added to `GatherFocusables` while it shows.

### Session browser (`UI/ApexSessionBrowserWidget.cpp`)

Drive > Browse sessions: the live sessions from the latest `LobbyState`,
filtered by the pills under the car bar (F or pad X steps them; Shift+F
back): All, Joinable (not full, not over), Racing (a race under way) and
Recorded (`ApexBrowser::Passes`). Recorded lists the server's recordings of
finished sessions from its health port (`LobbyState.recordings_port`,
`GET /recordings`, [operations](../server/operations.md); R fetches again):
the circuit's catalog name (never the file's `name`), when, kind, cars, laps,
length and winner, badged Recorded, In replays (already fetched) or Old (from
before streams, not watchable). Enter on one fetches the `.apxs` into
`Saved/Replays/` (`/recordings/<file>`) and watches it like any replay.
The header has Back and Refresh (R, pad Y; the server also broadcasts every
two seconds) with the session and live counts; under it the car the player
would join in, Change car (car select, returning here) and Create session.
One row a session: the track's preview, its display name (the catalog's,
the server's name only without a row), host, drivers, kind, length and sky,
a damage rule other than full, the pit rule ("1 stop · two compounds"),
and a badge (Lobby, Starting, Racing, Full, Finished, or Between sessions
for a race weekend waiting for its host, which still takes drivers). Enter
on a row joins in the pending car (SelectCar, then JoinSession; without a
car it opens car select; a full or finished session says so). Right from a row is its Watch
button, lit while a race is being driven (`WatchSession`). Rows are rebuilt
only when sessions come or go and refreshed in place otherwise, so a
broadcast never moves the pad's place.

### Main menu (`UI/ApexMainMenuWidget.cpp`)

Two columns. The left "hero" shows what the player was last doing (pending
track and car from the profile) with **Start session** (a one-click start of
the remembered session) and **Change setup** (the create screen). The right
"rail" is a tree of pages:

- **Garage**: Cars (Manage car setups opens the hotlap garage's setup sheet
  over the menu, `UApexRootWidget::OpenSetupEditor`; View car stats turns
  the list's detail panel into the highlighted car's stat sheet and back:
  horsepower, power to weight, engine, redline, tyre window, liveries,
  compounds, saved setups and the quickest lap on one), Tracks (the
  globe page, [below](#tracks-globe-uiapextrackglobepagecpp)). The Cars
  page turns the hero into a list of cars with a detail panel.
- **Drive**: Create session (Quick = the remembered session; Race weekend
  opens the create screen on its Race weekend mode; Custom = the create
  screen), Hotlap (the create screen on its Hotlap mode), Browse sessions
  (live sessions: watch or join), Connect to server.
- **Watch a race**: Live race (the menu's backdrop race full screen,
  `WatchBackdrop`) and Replays (the replays screen, with the count of saved
  files; Back from it lands on this page).
- **Driver profile**: the Profile screen
  ([below](#driver-profiles-apexprofilesh-uiapexprofilewidgetcpp)).
- **Settings**: the settings overlay.

While the server holds a seat for the player in a session they lost the
connection to (`UApexNetSubsystem::HasRejoinOffer`,
[sessions.md](../server/sessions.md#reconnecting)), a banner over the hero
says "You are still in a race at <circuit>" with "Click here to rejoin"
(`RejoinSession`), and beside it Give up seat (`GiveUpSeat`). It takes
focus when it appears (Enter or pad A rejoins); Right moves to Give up seat
and on to the rail, Up from the hero's actions reaches it and Down leaves
it. It goes when the offer does: a join, giving the seat up, a lost
connection, or the session no longer listed.

Keys: Tab / shoulders cross columns, Back (pad B) steps up a page, Escape
opens a "Back to main menu / Exit game" overlay, pad Start (or the pause key)
opens settings, Alt+Enter cycles fullscreen / borderless / windowed through
the settings subsystem (it never activates the focused row). Session create,
car select and replays return false from `WantsLiveBackdrop`, so the
backdrop world is hidden behind them (the car turntable shares the world).
In a session the lobby and results screens show the session's circuit
instead of the demo ([cameras.md](cameras.md#lobby-view-beginlobbyview-apexlobbycamfflyover)).

### Driver profiles (`ApexProfiles.h`, `UI/ApexProfileWidget.cpp`)

Several people can share an install. A **profile** is a name, a picture
and a driver id (an `FGuid` made when the profile is created), with two
save slots of its own: the profile (`UApexProfileSave`: selections, create
screen, best laps, stats) and the settings (`UApexSettingsSave`). The list
is `ApexProfiles.sav` (`UApexProfileIndexSave`: each profile's id, name,
picture and slots, and the one used last). No account, no service: it
lives on this machine only.

- **First run on an old install**: `ApexProfiles::LoadIndex` finds no list
  and makes the existing `ApexProfile` / `ApexSettings` slots the first
  profile, under its saved name. A new profile gets `ApexProfile_<id8>` /
  `ApexSettings_<id8>`, its settings copied from the profile in use
  (`UApexSettingsSubsystem::UseSlot`) and no history; at most 16.
- **Which one drives**: the one used last, or `-ApexProfile=<name or
  1-based place>`. With two or more profiles a launch opens the Profile
  screen as "Who is driving?" first (`ConsumeProfilePrompt`), unless the
  command line chose one or carries an `-Apex` flag that drives the game
  (an unattended run); `-ApexScreens=` and `-ApexContent*` do not count.
- **The driver key**: each profile holds an Ed25519 private seed
  (`UApexProfileSave::DriverKeySeed`, made with the profile or the first
  time an older one is used; never sent), so a backup of the save keeps
  the driver and a lost install is a new one. `ApexDriverKey`
  (ApexSimNet, OpenSSL) gives its public key and signs; the net subsystem
  sends the public key with the id and answers the server's challenge
  with `ProveDriverKey` before anything else
  ([protocol.md](../server/protocol.md#connection-flow)).
- **On the wire**: `Authenticate.driver_id` carries the id
  (`UApexNetSubsystem::SetDriverId` / `SetDriverKey`, set by the flow
  subsystem before the first connect), so the server knows the driver across connections: grid
  references (`@driver:`), stored qualifying lines and lap records
  ([sessions.md](../server/sessions.md#starting-order-and-qualifying)).
  Switching profiles or renaming one reconnects as the new driver (leaving
  any session); the resume token is kept per server and profile, so one
  profile never takes another's held seat. A stored qualifying result
  grids its humans by `@driver:` (`ApexCreateSession::OrderFromResult`,
  the host by their own id), labelled with the name they qualified under;
  the leaderboard lights the profile's own laps by id.
- **Stats** (`FApexDriverStats`): laps, distance, sessions, races (taken to
  the flag), wins and podiums (against at least one other car), incidents
  (penalties plus laps struck for track limits) and the best lap per
  circuit and car. `UApexSessionRecorder::FinishRecording` files each
  session the player drove (`ApexProfiles::FileSession`).
- **The Profile screen** (`EApexScreen::Profile`; main menu > Driver
  profile): the drivers on this machine (picking one switches to it; New
  driver makes one with the next unused picture; Delete, or pad Y, twice
  on another driver removes them and their saves), and the driver in use:
  their name (typed, committed on Enter or leaving), the 16 pictures, the
  stats and the personal bests. The main menu's top bar shows the picture
  beside the name.
- **Pictures**: `face_<n>.png` from the content's `drivers/portraits`
  (`UApexPortraitSubsystem`; `apexsim-content/drivers/portraits` in the
  editor, `Drivers/portraits` beside the packaged game, copied by
  `build_release.ps1`), read at runtime on first use.

Checks: `ApexSim.Net.DriverKey` (OpenSSL signs the server's own known
answer), `ApexSim.Profile.*` (stats filing, the list and `-ApexProfile=`,
the pictures there), `ApexSim.UI.CreateSession.StartOrder`'s profile
lines, `ApexSim.Leaderboard.Rows`, `ApexSim.Net.Protocol.*` for the wire.

### Tracks globe (`UI/ApexTrackGlobePage.cpp`)

Garage > Tracks takes the hero's and the rail's room: the circuits as a
list on the left (Back above it), a globe in the middle and the chosen
circuit's panel sliding in on the right.

- The globe (`UI/ApexGlobeWidget.cpp`) is painted: land as dots from a
  2.5° mask (`UI/ApexGlobeLand.h`, one row a band, cells equally wide on
  the ground), a graticule every 30°, the atmosphere's rim and an ocean
  gradient (transient textures). The maths are `UI/ApexGlobe.h`:
  orthographic projection, fly-to, drag with a throw, eased zoom (0.85x
  to 5x), and an idle spin that stops under the pointer, while the keys
  or pad are on the globe, and while a circuit is chosen.
- A pin per circuit whose catalog row `bHasGlobePosition` (the export's
  `metadata.latitude_deg` and `longitude_deg`; a track without them, such
  as a custom one with no location, is in the list only). Its head stands
  1.085 radii out, it shows only facing the viewer and inside the box,
  fading in at the limb. Labels show the catalog's display name, placed
  front-most first and skipped where they would overlap; the chosen, the
  hovered and the keys' pin always show theirs. The chosen pin is in the
  live green with a pulse.
- Choosing a circuit (its row, its pin, or `apexsim.ui.Globe select`)
  makes it the pending track, flies the globe to it at zoom 2.4 and opens
  the panel: the circuit's map (its preview picture), name, country /
  city / class (`ApexCatalog::DisplayClass`), description, length, your
  best, and Hotlap (the create screen on its Hotlap mode), Track guide
  (locked when the circuit has none), Watch hotlap and Leaderboards. The
  page hands these to the main menu by the rail's action ids
  (`OnAction`), so they start the flows the rail did.
- Mouse: drag turns and throws, the wheel zooms, a click on a pin or its
  label chooses it; + / - / Reset in the corner. Keys and pad on the
  globe: the arrows or D-pad hop to the nearest pin that way (Left off
  the last one goes to the list, Right to the panel), Enter or A chooses,
  the left stick turns, the triggers or + / - (PageUp / PageDown) zoom,
  Y or Home resets. In the list Up and Down step the rows and Right goes
  to the globe; Tab and the shoulders cycle list, globe and panel. Back
  (B, Escape) closes the panel, and with it shut leaves the page (Escape
  then opens the main menu's Esc menu).

### Car and track select (`UI/ApexCarSelectWidget.cpp`, `UI/ApexTrackSelectWidget.cpp`)

Moving through the list (car select) or the grid (track select) describes
each entry as it is reached. A key or pad A on a row or card chooses it at
once and leaves for `ScreenAfterCarSelect` / `ScreenAfterTrackSelect`; a
click only selects it, so the mouse can look at a car, step its livery or
read a track first, and Confirm (`bMouseOnly`) or a double-click chooses
it. Back leaves without changing the pending car or track. The button and
the card tell their handler what fired them (`GetLastActivation`:
`EApexActivation` Key, Click or DoubleClick; Slate sends a double-click's
second press as its own event, so it activates twice, as before). The
track search box takes every key it is sent, so the screen catches a
pad's D-pad (and Up and Down) on the way down
(`NativeOnPreviewKeyDown`): Left to Back, Right to the chips, Down to the
grid; the keyboard's Left and Right stay the caret's.

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
`window_mode`, `vsync`, `frame_limit`, `screens`, `graphics_api`), `server` (`host`, `port`,
`tls`, `tls_verify`, `tls_fingerprint`), `telemetry_out` (`enabled`, `host`, `port`, `rate_hz`: the
[telemetry output](telemetry-output.md)) and `launcher` (`show`). `UApexBootSettingsSubsystem` (`ApexBootSettings.h`)
reads it at game-instance startup, creates it when absent (seeded from the
desktop's display mode, or from the save slots on an existing install) and
rewrites it when those values change in game.

- Location: next to the executable. `Game/settings.yml` in a release,
  `game-unreal/settings.yml` (gitignored) in the editor. In a package
  `FPaths::ProjectDir()` is `<Release>/Game/ApexSim/`, so the file is in its
  parent. `-ApexSettingsDir=<folder>` reads and writes `<folder>/settings.yml`
  instead: a test run against its own server (a scratch folder with
  `server: port: 9100`) neither reads nor rewrites the player's file.
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
- `display.graphics_api: auto | dx12 | dx11` (default `auto`) picks the
  renderer. The engine chooses it from the command line in `RHIInit`, before
  any game module with UObjects loads, so `ApexSimBoot` (PostConfigInit)
  reads the key with its own line scan (`ApexGraphicsApi`) and appends
  `-dx12` / `-dx11`; an RHI switch already on the command line wins, and the
  editor and commandlets ignore it. The game only carries it through
  rewrites (`GraphicsApi`). `dx11` runs the SM5 shaders (no Nanite, no
  virtual shadow maps): the fallback for a GPU or driver that renders
  wrongly under DX12. The launcher sets it in Edit configuration > Graphics
  API. The engine's `LogRHI: RHI D3D11 ... will be used` confirms it.
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

## Graphics quality (`UApexSettingsSubsystem::ApplyGraphics`)

Settings > Graphics > Preset (LOW, MEDIUM, HIGH, ULTRA, CUSTOM) puts every
quality row in one engine scalability bucket, 0..3: Shadows, Anti-aliasing,
Textures, Lighting (`sg.GlobalIlluminationQuality` and
`sg.ReflectionQuality`), and a detail value with no row of its own
(`DetailQuality`: view distance, post-processing, effects, foliage,
shading). Changing a row by hand makes the preset CUSTOM, which keeps the
detail value of the last preset. A fresh slot is HIGH, but with Textures,
Lighting and detail at 3, the engine's own default for those groups.

Render scale (50-100%, `RenderScalePct`, in 5% steps) draws the 3D scene
at that share of the window and lets the engine's temporal upscaler (TSR,
the project's anti-aliasing method) bring it back to full size
(`UGameUserSettings::SetResolutionScaleValueEx`); the HUD and menus are
drawn at full size. The presets set it: LOW 70, MEDIUM 85, HIGH and ULTRA
100; moving it by hand makes the preset CUSTOM. HDR (`bHdrOutput`,
`EnableHDRDisplayOutput` at `HdrPeakNits`, 1000) is a row only where the
display reports HDR support (`SupportsHDRDisplayOutput`).

LOW is the path for integrated graphics. Lighting 0 turns Lumen off (no
Lumen GI, no Lumen reflections, no distance-field AO). Shadows 0 turns off
distance-field shadows and swaps virtual shadow maps for cascaded shadow maps
(`r.Shadow.Virtual.Enable` at game-setting priority over the project's 1).
MEDIUM keeps cheap Lumen GI and turns Lumen reflections off. On a first run,
and on Reset to defaults, a GPU the RHI reports as integrated
(`GRHIDeviceIsIntegrated`) starts on LOW.

Nothing in the project renders on the CPU. D3D12's WARP adapter (Microsoft's
software rasterizer) can run the engine, but far too slowly to drive. The
release targets Windows 10/11 x64 with a DirectX 12 GPU; Windows on Arm is
not supported ([follow-ups.md](../follow-ups.md)).

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

## Client prediction (`Race/ApexPredictionSubsystem.h`)

The local car can be drawn from the server's own car step, linked into the
client (the `ApexPhysics` module, [building.md](../building.md#the-physics-library-serverffi)),
stepped ahead of the server by the round trip and reconciled with every
state the server sends back
([proposals/client-prediction.md](../proposals/client-prediction.md)). Off
by default.

- **Switches.** `apexsim.net.Predict` 0 off, 1 on, 2 shadow (the loop runs
  and logs, the puppet is drawn); `-ApexPredict` (1) or `-ApexPredict=N`
  at start. `apexsim.net.PredictDebug 1`: an on-screen line (lead, round
  trip, rewinds, replay depth, the last correction, the visible offset).
  `apexsim.net.PredictLogSeconds` (5): how often it logs its figures.
  `apexsim.net.PredictServerDriven 1`: draw it while the server drives the
  car too (below), for unattended checks with the debug stand-in.
- **What it loads** (`UApexPredictionSubsystem`, on the session's join),
  off the game thread: the track's physics, with the session's conditions
  as `SessionJoined` carried them and the backend it names
  (`SessionJoined.Physics`); the car.toml from the car folders the game
  builds cars from, balanced as the server balances it when `Physics`
  names a `balance` (`apx_car_create_balanced`: the server's `[[balance]]`
  applied before the setup), the setup last sent (`GetLastSentCarSetup`,
  the car made again when a knob moves) and the grid slot. The track, from a server
  that names its physics bundle (`Physics.bundle_crc`,
  [server/protocol.md](../server/protocol.md#the-own-car-for-prediction)),
  in this order: this client's own YAML under the content root (`apexsim-content/`)
  (`-ApexContentDir=` elsewhere) and the sidecars beside it when their
  bundle checksum (`apx_track_files_crc`) is the server's; else the bundle
  cached by that checksum under `Saved/ApexSim/TrackBundles/<CRC>.apxtrack`
  (a refused one is deleted); else the server's bundle, fetched a chunk at a
  time (`FApexTrackBundleDownload`: 3 in flight, 6 requests a second after a
  burst of 10, failed after 10 s without a chunk), cached and loaded
  (`apx_track_load_bundle`, refused unless its checksum is the one named).
  The cache is kept under 64 MB (`ApexTrackBundleCache`): after a save the
  least recently used bundles go (a load from the cache counts as a use),
  never the one just saved.
  The join is never held for it: the puppet is drawn until the track is
  there. From an older server (no bundle) only the YAML on disk, its
  content checksum against the lobby's and the road mesh beside it when
  the server drives one. It stays off, and logs why once, without the
  library, from a server that sends no `Physics`, when the car.toml is
  missing or its content checksum is not the server's (a car the client
  has no folder for it cannot draw either, so the bundle carries no car),
  or when the bundle does not come. Every `RoadState` slice goes to the
  library's track (`apx_track_road_rows`; the ones heard while it loads or
  downloads are replayed onto it).
- **The loop** (`FApexOwnCarPredictor`, `Race/ApexOwnCarPredictor.h`), on
  the game thread each frame after the net subsystem:
  - The timeline is in server ticks, stepped one at a time at the
    session's rate with the server's tick numbers. A ring of 1.5 s keeps
    each tick's input and pose, and the full state at the ticks a snapshot
    lands on (the telemetry ticks, learnt from the snapshots).
  - Inputs are the UDP worker's numbered ones. Each is laid on the
    timeline at the tick the clock read when it went out and drives every
    tick until the next, as the server holds it. One heard after the
    timeline stepped past its place is written into those ticks and they
    are stepped again.
  - The clock targets the server's tick now (the earliest-arriving
    snapshot of a 2 s window, by the worker's arrival times) plus the round
    trip (from each snapshot's input number: between hearing it less the
    next input's sending and less its own, the middle, the shortest of the
    window) plus half a tick, plus an alignment trim: when the input the
    server held at a snapshot's tick is not the one the ring held there,
    the target moves by a quarter of the gap (at most 14 ticks). The clock
    slews toward its target by at most 5% and jumps when 0.25 s off.
  - A snapshot is laid over the ring's state of its tick (stepping there
    first if the timeline has not reached it) and the ticks since are
    replayed; a whole state blob is loaded as the state of its tick and
    replayed the same way, the newest snapshot laid again when the replay
    crosses it. The fields the session's passes own (pit lane, DRS allowed,
    headlights, wake) stay as the newest blob had them. The first blob
    seats the timeline; nothing is drawn before it.
  - What a rewind moved the head by becomes a correction offset decaying
    with a 0.1 s time constant; one over 20 m (the motion buffer's
    teleport) snaps. The pose drawn is the head blended back by the clock's
    remainder, plus the offset.
  - A snapshot that names no input, or the same one for 0.3 s while newer
    go out, means the server drives the car (the stand-in, an away driver):
    the replay then holds the controls the server last applied, and the
    round trip falls back to the heartbeat's ping plus the fake latency.
- **Drawing.** `AApexRaceCarActor` takes the predicted location, rotation,
  velocity, speed and steering for the local car, and from
  `GetOwnCar` its revs (blended across the sub-tick), throttle, brake and
  gear: the dash, the brake lights and the engine sound (own and world)
  follow them. The camera and the cockpit follow the actor.
- **Feel.** Each step's feedback comes out of the library from the server's
  own collector (`apx_car_feedback_take`) and is kept in the ring with its
  tick. Every frame the subsystem folds the ticks the clock reached since
  the last (`CollectFeel`, `FApexDriverFeedback::Absorb`, as a frame's
  messages are folded) into the feel `GetOwnFeel` hands out, with its own
  serial. While it draws the car, the force feedback
  (`AApexPlayerController::ReadDrivingSignals`: the feel, the predicted
  speed, gear and revs) and the tyre and kerb sound
  (`AApexRaceDirector::UpdateCarAudio`) read it instead of the server's
  `DriverFeedback`; the HUD's dash (`car.speed*`, `car.rpm*`, `car.gear*`,
  `car.throttle`, `car.brake`, `car.steering`) reads the predicted car
  through `ApexHudData::ApplyPredicted`. The torque is worked out at the
  input being sent, so the wheel's latency correction
  (`steer_stiffness`) is left with almost nothing to add. What stays the
  server's: the tyres' heat, damage and the ground under the wheels (they
  move over laps, from telemetry), the road texture's station, and on the
  HUD the lap, places, gaps and radar, which are read against the other
  cars and the lap clock. A switch between the two sources is not a new
  message, so no hit is played twice. With prediction off, or the puppet
  drawn, every reader takes the server's figures exactly as before. The puppet is drawn instead
  outside the race proper (the countdown, after the flag), in the pit lane
  (the limiter and the autopilot are the server's), in the garage and a
  recovery, while the server drives (unless `PredictServerDriven`), and
  when no snapshot came for 0.5 s; a switch either way blends from where
  the car was drawn over 0.15 s. Other cars stay on the motion buffer.
- **Figures.** Every 5 s while it runs the log has a line: snapshots,
  rewinds (and from a whole state), the replay depth's mean and largest,
  the correction's mean, p50, p95 and largest in mm, re-seats, clock
  jumps, the lead, the share of frames drawn from it (and why not, else),
  how far the puppet trails it (m and ms), the round trip and the
  alignment trim. Corrections count only while it would draw. A second
  line: how long before the server's own `DriverFeedback` (and snapshot)
  of a tick this client felt that tick's torque (and heard its revs), and
  how far apart the two were (gathered in shadow too). Loading says where
  the track came from, and a fetched bundle its size, chunks and seconds.

Tests: `ApexSim.Prediction.*` on a recorded server sequence
(`Tests/ApexPredictionFixture.h`, written by `server/ffi/tests/client_fixture.rs`;
regenerate it after a physics change or an edit to Monzarella.yaml or the
fugazzi-sf26 car.toml: `cd server/ffi && cargo test --test client_fixture -- --ignored --nocapture`):
`Library` (linked, the ABI, the fixture), `ReplaysTheServer` (the library
from the server's blob with its inputs lands within 1.4 mm of each
snapshot over 2 s), `SnapshotRewind` (a snapshot over a wrong state
replays 42 ticks within 6 mm; without it 0.5 m off), and the loop on the
sequence as a client at a ping hears it: `Loop.AtPing` (0, 80, 160 ms:
correction p95 under 1 mm, largest 3.5 mm at 80 ms; the replay reaches back
the round trip), `Loop.Loss` (every third snapshot lost), `Loop.Contact`
(the server's car pushed 0.3 m: one correction, blended away), and
`Loop.Relocation` (300 m: a re-seat, no blend); `Feel.ReplaysTheServer`
(each tick's feedback from the library, folded per telemetry tick, against
every `DriverFeedback` the server sent in the window), `Feel.AtPing` (at
80 ms the torque and revs lead the server's by about the round trip and
are its figures; the dash the HUD and the engine read is the server's at
each snapshot; `ApexHudData::ApplyPredicted` changes only the dash),
`Bundle` (the fixture's track bundled as the server bundles it, through a
cache file: a car on it steps as on the files, bit for bit; another
checksum or a damaged byte is refused) and `BundleDownload` (pacing,
order, another bundle's chunks ignored, a missing bundle or a stall
fails). Without the library they pass with a warning.

## Lap traces (`Race/ApexLapTrace.h`, `UApexLapTraceSubsystem`)

Every whole lap the player drives is recorded from their own car's
telemetry (`FApexLapRecorder`): a sample every 2 m of station with the lap
clock, speed, throttle, brake, steering, gear, revs, the four treads and
the position. A lap counts only when it was seen from the line (the first
sample within 30 m and 2 s of it) to the next crossing with no jump over
100 m: an out-lap, a lap joined part-way, a recovery, a stop with service
or a rejoin give none. The station may wrap a few frames before the lap
counter; that is not a jump. Spectating, backdrops and replays record
nothing.

Each lap is written as CSV to `Saved/Telemetry/<circuit>/<car>/`
(`<date>_lap<N>_<m-ss-mmm>[_invalid].csv`, a `#` header with the circuit,
car, lap, time and validity, then a row per sample;
`ApexLapTrace::ToCsv` / `FromCsv`). The subsystem keeps the last lap, the
best valid one (this session's or the quickest stored for the circuit and
car) and every lap held for them (`GetLaps`, newest first: this session's
and the newest `MaxListedLaps` (40) stored, the best kept whatever its
age). A lap the recorder drops is logged with why (`Lap trace: lap N
broken at ... m: serviced`, a jump, or not kept at the line).

The pause menu's LAP TRACES row (shown once there is a lap) widens the card
to `UApexLapTraceWidget`: speed, throttle and brake, and the time one lap
loses or gains on another (`ApexLapTrace::DeltaByDistance`) by distance.
Two steppers pick the laps from `GetLaps`: LAP (drawn in the accent
colour; the last, or the one before when the last is the best) and
AGAINST (muted; the best), each read out as lap, time and BEST / INV, with
both laps' times and dates, the gap and the folder above.
`apexsim.telemetry.Laps` logs what is held and the lap being recorded.

`scripts/motec_export.py <lap.csv>... [-o out.ld] [--rate 20] [--driver
NAME]` turns one or more of these CSVs, in driving order, into a MoTeC i2
log: an `.ld` with the samples resampled on time (Ground Speed, Throttle
Pos, Brake Pos, Steering, Gear held between samples, Engine RPM, the four
Tyre Temp, Lap Distance, Car Pos X / Y, Lap Number; float32 at `--rate`
Hz, the laps back to back) and an `.ldx` beside it with a beacon at each
line and the fastest lap. `read_ld` reads one back; tests:
`python -m unittest discover -s scripts/tests -p "test_motec_export.py"`.

Tests: `ApexSim.LapTrace.*` (whole laps only, a wrap ahead of the counter,
a recovery's jump; the CSV round trip and the delta).

## Racing line overlay (`Race/ApexRacingLineActor`)

Settings > Assists > Racing line: OFF / BRAKING ONLY / FULL (default off;
the host can forbid it, see [sessions.md](../server/sessions.md)). The server
builds the line per car (`server/sim/src/racing_line.rs`: the track's raceline or
centerline with a quasi-steady-state speed profile) and sends it as
`RacingLine` after `SessionJoined`, planned for the stock car with a dry
tank. At each lap's end it plans the line again for the car as it is
(`GameSession::refreshed_racing_line`: the driver's setup, the next tenth of
the tank above what they carry, and `CarState::tyre_grip_share`, which reads
the compound, wear, tyre heat and the road under the car) and sends it when
the setup or the tank step changed or the grip left its 0.02 step; the
client keeps the dots where they lie on the road and only repaints them
when the new line has the same points. `AApexRacingLineActor` draws dots, one
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
`-ApexMode=<EApexGameMode>` (6 qualifying, 7 race, 8 hotlap),
`-ApexQualifyingMinutes=N` (0 open), `-ApexWeekend=P,R,M` (a race weekend:
P minutes' practice, 0 for none, R knockout rounds of M minutes; the race is
`-ApexLaps` / `-ApexRaceMinutes`, and the start is the weekend's first
session), `-ApexNoStart`
(stop in the lobby), `-ApexAiSkill=N`, `-ApexDamage=off|reduced|full|visual`,
`-ApexLockAssists=abs,tc,gearbox,steering,line`, `-ApexQualifyingCutoff=N`
(the cut-off, percent of pole; 107 is the rule), `-ApexMandatoryStops=N`
(0-3) and `-ApexCompoundChange` (the race's pit rule),
`-ApexQualifyingFormat=shootout`, `-ApexLapAllowance=N`, the race-control
rules `-ApexRollingStart`, `-ApexFormationLap`, `-ApexPenalties` (jump
starts, yellow passes, the pit exit light), `-ApexSafetyCar=virtual|on`
and `-ApexReverseGrid=N`,
`-ApexGridOrder=@host,@ai:1,...` (the start order), sky
`-ApexWeather=sunny|cloudy|overcast|lightrain|heavyrain|snow`,
`-ApexTimeOfDay=HH:MM`, `-ApexTimeScale=N`, `-ApexChangeable=N`,
`-ApexTrackRubber=N`, `-ApexDayOfYear=N`, `-ApexForecast=MIN:weather,...`
(a written forecast). In the race: `-ApexView=cockpit|chase|tv|roof|close|near|far`,
`-ApexRacingLine=...`, the shot camera switches ([cameras.md](cameras.md)).
Hotlap: `-ApexHotlapOutAfter=N`, `-ApexHotlapGarageAfter=N`,
`-ApexHotlapReplayAfter=N` (comma lists).

### Watching, replays, guide, content folders

- `-ApexWatch` (the backdrop), `-ApexWatchSession`,
  `-ApexWatchReplay=<file|latest>`, `-ApexWatchHotlap`, with
  `-ApexWatchCamera=`, `-ApexWatchTower=`, `-ApexWatchCar=`,
  `-ApexWatchHideHud`; console `apexsim.watch [...]`. See
  [spectator.md](spectator.md).
- Backdrop: `-ApexNoDemo`, `-ApexNoShowcase`, `-ApexShowcase=<file|id>` (an
  id the game renders first when it has no file of it), `-ApexShowcaseDir=`.
- `-ApexReplay=<file>.apxs` and the `-ApexReplay*` family (record, fps, camera
  and shot options) play a clip with no server; see [spectator.md](spectator.md)
  and [marketing-site.md](../marketing-site.md).
- The benchmark: `python scripts/benchmark.py [--track SpaFrankenchamps] [--res 1920x1080]`
  renders one seeded race (12 GT3s, sunny, `apexsim-replay render --seed 7`)
  to `build/benchmark/<Track>.apxs` once, and plays it with
  `-ApexReplayBench[=<file>]`: from `-ApexReplayStart` for
  `-ApexReplayDuration` (20 s, 60 s) on the TV director's seed, every
  frame's real time kept, then the mean fps, the 1% low, p50 / p95 / p99 and
  the worst frame with the GPU, RHI and resolution to the log and
  `Saved/Benchmarks/<Track>_<time>.json`, and the game exits. An RTX 3080 at
  1920x1080: 104-107 fps mean, p50 9.3-9.6 ms, p99 11.6-12.1 ms (2026-10-17);
  the 1% low moves with the first run's shader hitches.
- `-ApexGuide=<Stem>[:Class]`, `-ApexGuideCorner=N`, `-ApexGuideCamera=N`;
  see [track-guide.md](../../../apexsim-content/docs/track-guide.md).
- Content folders: `-ApexTracksDir=`, `-ApexCarsDir=`, `-ApexHudDir=`, `-ApexGuideDir=`.

### Pad walk (`UI/ApexPadWalk.h`, `ApexSim.UI.PadWalk`)

Walks every menu screen and overlay by pad alone: main menu, connect,
session browser, create session, car select, track select, session lobby,
loading, results, replays, settings, pause menu, car setups, HUD editor. For each it checks that focus
lands on a control, that the D-pad and the right shoulder (pages and tabs)
reach every control a pad could focus, and that pad B leaves where it should
(not the main menu, lobby or results). The HUD editor's D-pad moves panels,
so its stop presses through its own pad map instead (`FPadWalkStop::Script`:
RB picks a panel, A/B the add list, R3/B the layout manager, L3 resets) and
closes without saving. Keys go through `FSlateApplication`
as a pad's do, one control a frame, so scrolled rows and new pages are laid
out as for a player. Accept is never pressed and Left/Right never on a
slider, so nothing is chosen; a car's livery may change.

It needs the running game and real rendering (Slate's layout is empty under
`-nullrhi`); in the headless editor run it passes with a note. A server on
the configured address fills car and track select:

```powershell
& "$UE\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "$PWD\game-unreal\ApexSim.uproject" -game `
    -RenderOffscreen -ResX=1920 -ResY=1080 -ApexNoDemo -ApexNoSplashHold `
    -ExecCmds="Automation RunTests ApexSim.UI.PadWalk; Quit" -unattended -nosplash -log
```

`-ApexPadWalkOnly=<name>` walks the stops whose name contains it;
`-LogCmds="LogApexPadWalk Verbose"` logs every step (from, key, to, where).
`apexsim.ui.PadWalk` walks whatever is in front in a running game. A
control the walk cannot reach fails the test with its name and place: give
the surface a way there (`HandleNavigation`, `ApexNav::MoveToward`) or, when
a pad does its job another way, make it `FApexButtonSpec::bMouseOnly`.

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
- Demo: `apexsim.demo.Enabled|MaxMinutes|Restart`,
  `apexsim.spectate.Info|Next`.
- Cars: `apexsim.car.Rescan`, `apexsim.car.Interp*`, `apexsim.car.DamagePreview "F,R,L,Rt,E"`,
  `apexsim.car.GroundPreview 0-3` (every car's tyres on gravel, sand or the desert floor: its dust),
  `apexsim.car.DamageEffects`, `apexsim.car.HeadlightLumens|BrakeLightNits|TailLightNits|WheelMaxDegPerFrame`.
- Tracks and lighting: `apexsim.track.Rescan|KeepLast|BuildBudgetMs|FlatShadows|HorizonShadows`,
  `apexsim.sky.SunStepDeg`, `apexsim.lights.Max|Range`.
- HUD: `apexsim.hud.Reload|Data|Edit|EditStep` ([hud-modding.md](hud-modding.md)).
- Hotlap: `apexsim.hotlap.Out|Garage|Replay|Stop|Tab|Board|Watch`.
- The race's grid: `apexsim.grid.Ready|Livery|StartNow`.
- Replays: `apexsim.replay.Save|List`; on a `-ApexReplay` clip
  `apexsim.replay.Seek <s>` puts the clock at a second and
  `apexsim.replay.Hold [on|off]` stops it there while the cameras go on
  (a close-up of a tread or a dent, unattended through `-ApexExecAfter`);
  watching (the backdrop, a client replay, a timeshift) `apexsim.watch
  hold|play|seek <s>` does the same. Guide: `apexsim.guide.*`.
- Display: `apexsim.view.Screens`, `apexsim.splash.MaxSeconds`.
- Menus: `apexsim.ui.Texts|Click|Mouse|Key` (above), `apexsim.ui.PadWalk`;
  `apexsim.ui.Globe select <stem|id> | close | zoom <z> | view <lon> <lat> [zoom] | home | spin 0|1 | info`
  drives the Tracks page's globe once it is open (run `m2-tracks-globe`).
- Race: `apexsim.recover [track|pits]` (the pause menu's BACK TO TRACK / PITS),
  `apexsim.finish [view|car N|panorama|end]` (the view after the flag, now).
- Input and audio: `apexsim.input.Devices|Rescan`, `apexsim.ffb.Debug`, `apexsim.audio.RenderCars`.
- Net: `apexsim.net.ParseCenterline` (parse the lobby's track centerlines; set from HUD detail),
  `apexsim.net.Drop` (close the connection as a lost network would, no
  `Disconnect` sent: the reconnect and, driving, the automatic rejoin,
  unattended), `apexsim.net.FakeLatencyMs N` (an artificial round trip on
  the UDP path, below; `-ApexFakeLatencyMs=N`).
- Telemetry output: `apexsim.telemetry.Out [on|off]` ([telemetry-output.md](telemetry-output.md)).
- GPU: `apexsim.debug.BoundsWatch` (on outside Shipping; `UApexBoundsWatchSubsystem`)
  checks every rendered primitive's bounds after each frame's actor ticks
  and logs a NaN, infinite or out-of-world one (`Bounds watch: ...` at
  Error, log flushed) with its owner and transform, plus each new largest
  primitive past 1 km. Size and distance are checked on shadow casters
  only (only casters reach the VSM culling), so the rain cloud dome's
  20 km shell passes; NaN is checked on every primitive. About 0.2 ms a frame for a thousand primitives. It
  is there for `DXGI_ERROR_DEVICE_HUNG`: a hang caught in VSM's non-Nanite
  culling (`CullPerPageDrawCommandsCs`) fits one primitive with NaN or
  absurd bounds. For a soak, `run_checks.py "b1-*" --repeat N
  --game-arg=-gpucrashdebugging` cold-starts the demo backdrop and tallies
  hangs and `BOUNDS` lines in `out/verify/soak.log`.

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
- Slate's scroll box steps a pad only between its own direct slots: with one
  vertical box inside it, Down from a row goes nowhere. Its other trap is
  layout: rows scrolled out of view are not arranged, so their cached
  geometry is stale or zero. Use `ApexNav::MoveToward`.
- A `UWidgetSwitcher`'s other pages are "visible" to `UWidget::IsVisible`;
  `ApexNav::CanFocus` treats them as hidden. Check focusability with it, not
  with visibility.
- A stepper (`UApexStepperWidget`) or segmented control is not focusable
  itself; focus its first pill (`ApexNav::FindFirstFocusable`).
- `UApexButtonWidget::Setup` (and `SetBadge`, which may call it) resets
  focusability from the spec: set `bMouseOnly` in the spec, not
  `SetIsFocusable` afterwards.
