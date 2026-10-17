# Roadmap

Product features, content and tooling that are not started yet. What is
left on a feature that is built (bugs, edge cases, cleanup) is in
[follow-ups.md](follow-ups.md); what is built but has not been seen, heard
or felt yet is in [verification.md](verification.md); simulation and
racing-rule gaps (physics, tyres, AI, pit and race rules, race weekends)
are in [simulation-gaps.md](simulation-gaps.md). An item lives in exactly
one of the four.

The backlog is a list of **work packages**. A package is one go: the items
in it touch the same code, the same wire change or the same content
refresh, so they are built on one branch, verified with one check and
committed together. Each package says what it is for, what is in it, and
how it is checked. A package is done when its items are built and its
check has been run. When an item is built, delete its bullet (anything it
leaves behind goes to [follow-ups.md](follow-ups.md) or
[verification.md](verification.md)); when a package has none left, delete
the package. Git history keeps what was done.

Ideas that have no scope or check yet are not packages; they wait under
[Not yet scoped](#not-yet-scoped) until someone writes them one.

Priorities (a package takes the priority of its most urgent item), and
within a priority the packages are in the order to do them:

- **P0**: blocks someone from playing a race start to finish. Build before anything else.
- **P1**: core sim-racing experience. Players notice at once when it is missing.
- **P2**: depth that keeps players coming back.
- **P3**: nice to have.

## P0: playable end to end

None open.

## P1: core racing experience

### 3. Client-side prediction for the local car

90-150 ms from wheel to car online. Design and phases in
[proposals/client-prediction.md](proposals/client-prediction.md).

- The physics crate as a staticlib behind a C FFI.
- An input sequence on `PlayerInput`, a local-car snapshot on
  `DriverFeedback` (golden bytes both sides).
- The predict / rewind / replay loop behind a console switch, an
  artificial-latency harness.
- The track physics bundle served on join, keyed by `ContentCrc`.

**Check:** the FFI and the server give equal states on a recorded input
trace; a driver at 80 ms artificial latency.

## P2: depth

### 4. Formation lap and rolling starts

- Start type (standing / rolling) and formation lap as `CreateSession`
  rules echoed in `SessionJoined`; the field follows in grid order at a
  capped speed; `FORMATION_LAP_WARMTH` goes once the lap warms the tyres.
- A jump start recorded (penalised with the penalties gap).

**Check:** session tests for both start types; the AI survey with rolling
starts; a race start on screen.

### 5. Multi-class racing

- A field per class (cars, count, AI level) in `CreateSession`; position in
  class on the tower, results and records by class; class-aware blue
  flags; the AI yielding and passing across classes.

**Check:** the AI survey with a two-class field; results by class on screen.

### 6. Telemetry logging and lap comparison

Built: every whole lap recorded and saved as CSV, the pause menu's last
against best by distance ([game/client.md](game/client.md#lap-traces-raceapexlaptraceh-uapexlaptracesubsystem)).
Left:

- Comparing against the ghost or the hotlap record (the server's
  `GhostLap` traces are 20 Hz positions only), and picking any two stored
  laps.
- MoTeC i2 (`.ld`) export.

**Check:** an export opened in MoTeC.

### 7. Championships

- Offline first (online leagues later, on the race weekends' persistence
  and identity): a season of rounds (track and session rules) against an AI
  field with fixed names and levels, a points preset (F1-style top ten, or
  points to every finisher), an optional fastest-lap point and drop rounds,
  driver and team standings, saved in the profile.

**Check:** a three-round offline season start to finish.

### 8. Public server list

- A small self-hosted Rust service (a Dockerfile beside the server's)
  that servers register with and heartbeat to over HTTPS (opt-in per
  server, TLS fingerprint, verified by connecting back); an Internet tab in
  the session browser.

**Check:** two servers listed, one going away; joining from the list.

### 9. Text chat

- `Chat` messages on TCP, rate- and length-limited by the server, in the
  lobby and as a HUD component in a race; admin mute.

**Check:** the chat wire tests; two clients chatting in a race without
losing pad focus on the viewport.

### 10. Upscaling and HDR

Built: a render scale row (TSR) set by the presets and an HDR switch
([game/client.md](game/client.md#graphics-quality-uapexsettingssubsystemapplygraphics)).
Left:

- DLSS and FSR as plugins (their licences), and a paper-white level for
  HDR.

**Check:** the presets on the RTX machine and an integrated GPU; mirrors
and triple screens at each scale; HDR on an HDR display.

### 11. Own liveries

- Import a texture against an exported UV template into
  `content/cars/custom`; later an in-game editor (colours, decals,
  number); online, liveries sent to the other clients, size-capped and
  cached by hash.

**Check:** a custom livery on the car in a race and on a second client.

## P3: nice to have

### 13. Rewind in offline practice

- Periodic full-state snapshots; rewind N seconds and resume; laps after a
  rewind invalid for records.

**Check:** a rewound run replays deterministically.

### 14. Spotter and engineer radio

- Voice cues for car left / right / clear, yellow, blue, fuel, tyres and
  the box call: a line list rendered offline with a neural TTS and shipped
  as audio files, a client cue queue with priorities and cool-downs.

**Check:** a race with the radio on, by ear.

### 15. Driver profile and stats

- Laps, distance, races, wins and personal bests per track and car on a
  profile screen; online keyed on the weekend identity.

**Check:** stats after an offline race and a hotlap.

### 16. Driving school

- Challenges on the racing line and corner data: brake at a board, a corner
  under a target, a clean lap under time; medals.

**Check:** each challenge passed and failed by the stand-in.

### 17. League rules

- Reverse top-N grid, success ballast by championship position, balance of
  performance per car, each a `CreateSession` rule; a car without the keys
  simulates as before.

**Check:** session tests per rule; the class calibration unchanged.

### 18. Results webhooks

- An optional webhook on finish, posting the classification the server
  already writes (`records/results/`); needs certificate roots for HTTPS.

**Check:** a webhook against a test endpoint.

### 19. Incident review

- An incident list (contacts, passes under yellow, track limits) with a
  jump to the replay time, for admins.

**Check:** a seeded pile-up's incidents listed and opened.

### 20. Photo mode

- Pause, free camera, focal length, depth of field, exposure, no HUD, a
  high-resolution shot.

**Check:** a shot from a paused replay.

### 21. Benchmark from the menu

- The benchmark (`scripts/benchmark.py`, `-ApexReplayBench`) from the main
  menu, with the clip shipped or rendered on first use.

**Check:** a run from the menu and its numbers on screen.

### 22. Colour-blind HUD

- Colour-blind palettes through the HUD's colour tokens, and shapes or text
  wherever colour alone carries meaning (wear, brakes, flags, delta).
- UI strings moved to string tables as screens are touched; the game stays
  English until there is demand.

**Check:** the HUD under each palette and a colour-blindness simulator.

### 23. VR

- OpenXR cockpit view, world-space HUD panels, a VR pointer for menus,
  mirrors within the frame budget.

**Check:** a race in a headset at a steady 90 Hz.

## Not yet scoped

Ideas without a scope or a check. Each needs a short design (what it is,
what it touches, how it is checked) before it becomes a package.

- **Driver rating / safety rating.** No definition yet of what is rated or
  where it is stored.
- **Shared leaderboards across servers.** Lap records and the track
  leaderboard screen are per server; sharing needs a service and a trust
  model.
- **UDP encryption.** Inbound datagrams are sealed (HMAC under a
  per-connection key from `AuthSuccess`, rising sequence), so forged,
  tampered or replayed input is dropped; only reading telemetry and input
  off the wire is left open, and it carries nothing the other players do
  not already get. TCP has TLS. Likely not worth DTLS.
- **More screens.** A five-wide row (`MaxSplitscreenPlayers` would need 5),
  a second monitor for telemetry, shared exposure across the triple-screen
  views.
