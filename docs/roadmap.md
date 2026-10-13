# Roadmap

Product features, content and tooling that are not started yet. What is
left on a feature that is built (bugs, edge cases, verification, cleanup)
is in [follow-ups.md](follow-ups.md); simulation and racing-rule gaps
(physics, tyres, AI, pit and race rules, race weekends) are in
[simulation-gaps.md](simulation-gaps.md). An item lives in exactly one of
the three.

The backlog is a list of **work packages**. A package is one go: the items
in it touch the same code, the same wire change or the same content
refresh, so they are built on one branch, verified with one check and
committed together. Each package says what it is for, what is in it, and
how it is checked. A package is done when its items are built and its
check has been run. When an item is built, delete its bullet (anything it
leaves behind goes to [follow-ups.md](follow-ups.md)); when a package has
none left, delete the package. Git history keeps what was done.

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

None open.

## P2: depth

### 4. Hotlap

The watch's seek, ghost and replay reuse the stream v2 controls
([game/spectator.md](game/spectator.md)).

- A multiplayer hotlap has an in-session board across drivers (the garage
  scoreboard is shown in qualifying only).
- Hotlap watch: picks beyond stock setup, tyre and sky (wind, air
  temperature, rubber, a saved setup); pause, seek and speed; a ghost and a
  replay.

**Check:** two clients in one hotlap session; a watch with each pick.

### 5. Replays carry everything

A stream's rows and `State` records leave out the ghost and the driver
feedback, so a replay is quieter and emptier than the race.

- Streams carry the ghost and the driver feedback (slip, slide and contact
  surface), so a replay has tyre and kerb sound.
- The promo pipeline records audio.

**Check:** a replay of a race and a hotlap side by side with the live run,
by ear; a promo clip with sound. (Frames stay whole: no delta encoding, see
[game/spectator.md](game/spectator.md).)

### 6. Kit pieces on track

Prop-kit pieces that no `ats-dress` pass lays yet: kerb markers, sausage
kerbs, the podium, the hillside letters (after their display-name fix in
[follow-ups.md](follow-ups.md)).

**Check:** each circuit by hand in the running game; the track-editor tests.

### 7. Nordschleife dressing

One circuit, a long list; its own package so it does not hold up the rest.

- Numbered marshal posts, the track-over-road bridges (B257 at Breidscheid),
  rock cuttings, spectator hedges and banks, camp villages, the timing
  gantry as a crossing, more German signs and the T13 gates, the Hohe Acht
  tower, vertical graffiti on barriers, skid-mark decals.

**Check:** a lap by hand and a TV-camera lap in the running game.

### 8. Menus

Shell screens. All of it goes through one pad walk
(`ApexSim.UI.PadWalk`) at the end.

- Session browser: recorded sessions and filters.
- Replay export.
- A car stats view (the row opens car select today).
- A direct jump into a track guide (it opens the track picker today).
- A HUD layout picker outside the HUD editor.

**Check:** the pad walk, then the same screens with a real pad.

### 9. Wheels and pedals

- A clutch pedal is sent.
- More wheelbase makers in `ApexWheelProfiles` (PXN, Gomez, Heusinkveld
  direct drives...); they mix as Generic today.

**Check:** the input tests; the hardware session in
[follow-ups.md](follow-ups.md).

## P3: nice to have

### 10. Car content

- The GT3 and LMP2 class wheels get sidewall lettering.
- The HUD colours brakes by material as well as temperature (the client
  learns carbon from steel from the `[brakes]` table).

**Check:** the HUD on a carbon and a steel car; the wheels in car select.

### 11. Server dashboard

For people running a server for others
([server/operations.md](server/operations.md)).

- Restarting the server process itself from the dashboard, for the config
  keys that need it.

**Check:** a saved tick rate in force without touching the machine.

### 12. Content tooling

- `ats-dress --explain` and a per-prop source stamp.
- `ats-export --strict`, which fails on a dropped facet.
- An aerial-imagery tracing aid for dossiers.

**Check:** the track-editor tests.

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
