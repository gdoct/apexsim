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

None open.

## P2: depth

### 1. Session rules on the create screen

Rules the server takes on `CreateSession` that no screen offers yet
([server/sessions.md](server/sessions.md#session-rules-set-on-create)).

- A race weekend's days: a sky picker for practice and for qualifying in
  the Race weekend section (`WeekendFormat.practice_conditions` /
  `qualifying_conditions`; golden `C_CreateSessionWeekendDays` is in
  `ApexGoldenBlobs.h`, the codec does not send it yet), with each day's
  forecast shown as the create screen shows the race's.
- Joining a race weekend between phases: the session browser offers Join
  on a weekend listed `Finished` while it waits for its host (the server
  takes the driver), and the client decodes `KnockedOutData.Late` so the
  timesheet and garage name a late entry rather than "out in Qn".

**Check:** `ApexSim.UI.CreateSession.*`; a session created with each new
rule echoes it in `SessionJoined` and the lobby shows it.

## P3: nice to have

### 1. Session date

- A Date row on the create screen's Conditions tab that sends
  `day_of_year` (the server and the client's sky take it already,
  [server/conditions.md](server/conditions.md)), and the tab's sun preview
  (`ApexCreateSession::Look`) at the track's solar noon and the date.

**Check:** a December create at Silverstone at 16:30 lights a dusk.

### 2. Car content

- The GT3 and LMP2 class wheels get sidewall lettering.
- The HUD colours brakes by material as well as temperature (the client
  learns carbon from steel from the `[brakes]` table).
- Disc wear on the HUD: `CarState::brake_disc_wear_pct` (a car with
  `[brakes] disc_wear_per_mj`) is server-side only; it needs a telemetry
  field and a data point beside `brake_wear`.
- The stint energy left on the HUD: `[fuel] stint_energy_mj` counts the
  whole car's energy per stint server-side only (`CarState::stint_energy_used_mj`);
  it needs a telemetry field and a data point beside `ers_stint_pct`.

**Check:** the HUD on a carbon and a steel car; the wheels in car select.

### 3. Server dashboard

For people running a server for others
([server/operations.md](server/operations.md)).

- Restarting the server process itself from the dashboard, for the config
  keys that need it (the config reply lists them today).

**Check:** a saved tick rate in force without touching the machine.

### 4. Gravel and sand on screen and in the ears

The server drives gravel, sand and the desert floor as their own grounds
(`OffGround`, [server/vehicle-physics.md](server/vehicle-physics.md)), but
the client only hears and feels "off" (`ContactSurface::Off`).

- A gravel or sand ground per wheel on the wire (an appended
  `ContactSurface` value or field), then a dust plume and a stone-rattle /
  sand-hiss layer in the road synth ([game/audio.md](game/audio.md)) and
  the force feedback's off-track texture per ground.

**Check:** a car run into a Monza gravel trap and onto Sakhir's floor,
seen and heard; golden bytes on both sides.

### 5. Content tooling

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
