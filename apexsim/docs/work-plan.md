# Work plan

A prioritised view over the four backlogs, snapshot of 2026-10-24 at
`50d69b4e`, with the COTA start/finish packages (roadmap 38-50) added
2026-10-25. What the plan's finished items brought is in
[features-by-date.md](features-by-date.md). The backlogs stay the source of truth for *what* is open
([roadmap.md](roadmap.md), [simulation-gaps.md](simulation-gaps.md),
[follow-ups.md](follow-ups.md), [verification.md](verification.md)); this
file says *in what order*, *what kind of work* and *who has to be there*.
When an item is done, delete it from its backlog and from here.

## Current state (2026-10-24)

Main is green at `50d69b4e` (server tests pass but for an occasional
`test_disconnect_cleanup` timeout under load; 352 client tests). Every
agent branch of the 2026-10-23/24 night run is merged, A4's squeeze
fix and A7's prototype measurements included. The imported F1s are measured (A7: their
contact is left). N7 is done (fastest-lap rule and per-round rules offline, standings in a round's lobby and results online).

## Legend

**Priority** (re-assessed across all four docs, not copied from them):

- **P1** — a race breaks, crashes, or a core sim-racing feature is missing or unverified. Do next.
- **P2** — visible quality, depth, or an enabler that unblocks several other items.
- **P3** — nice to have, low impact, or niche.
- **Parked** — no scope, blocked on outside data/hardware, or recommended to drop.

**Human** — what a person has to contribute:

- 🟢 **None** — an agent can build and check it end to end (tests, survey, unattended run judged from shots).
- 🟡 **Review** — an agent builds it; a human judges the result (screens, a feel, a design call) before it is closed.
- 🔴 **Required** — cannot be done without a person: hands on a wheel, ears, a specific machine, art, outside data, or a product decision.

**Effort** — S (a day or less), M (a few days), L (a week or more / multi-step).

## 1. At a glance

Sorted by priority, then by the step in [§2](#2-recommended-order) that
takes it up (— : no step, taken opportunistically).

| ID | Item | Type | Pri | Human | Effort | Step |
|---|---|---|---|---|---|---|
| H1 | Wheel, pedals and FFB sitting | Verification | P1 | 🔴 | S | 1 |
| B15 | Rim shakes violently at a standstill (follow-ups #31) | Bug | P1 | 🔴 | S | 1 |
| C5 | Circuit dressing to the references (roadmap 29): open cells and shots to judge | Content | P2 | 🟡 | L | 2 |
| C9 | The stadium stand's canopy and rail (roadmap 43) | Content | P2 | 🟡 | M | 2 |
| C10 | Horizon for open-country circuits (roadmap 44) | Content | P2 | 🟡 | M | 2 |
| H2 | Racing the AI, listening, pad sittings | Verification | P2 | 🔴 | S each | 3 |
| V2 | Verification runs R1, R2, R7 | Verification | P2 | 🟡 | S each | 3 |
| P3 | Integrated-GPU run | Platform | P2 | 🔴 | S | 3 |
| N10 | DLSS / FSR upscaling | Rendering | P3 | 🟡 | M | 2 |
| N29 | Broadcast HUD layout (roadmap 50) | Feature | P3 | 🟡 | S | — |
| G4 | Road and terrain outside Lumen's distance-field scene (A/B done) | Rendering | P3 | 🟡 | M | 2 |
| A7 | Imported F1s' AI touches other cars twice as much (follow-ups #19) | AI | P3 | 🟢 | S | — |
| G6 | Top view of formula cars looks cheap | Rendering | P3 | 🟡 | M | — |
| C2 | Circuit dressing backlog | Content | P3 | 🟡/🔴 | M–L | — |
| C3 | Content tooling backlog | Tooling | P3 | 🟢 | S each | — |
| S2 | Low-impact physics gaps | Simulation | P3 | 🟢 | M–L | — |
| A4 | AI depth: racecraft, strategy, hybrid, wet line | AI | P3 | 🟢 | M each | — |
| R4 | Small race and qualifying rules | Race rules | P3 | 🟢 | S–M each | — |
| V3 | Verification runs R4, R5, R10 | Verification | P3 | 🟡 | S each | — |
| F1 | Windshield wipers | Feature | P3 | 🔴 | M | — |
| AU1 | Audio overhaul | Audio | P3 | 🔴 | L | — |
| N22 | Localisation | Accessibility | P3 | 🔴 | M | — |
| N23 | VR (OpenXR) | Platform | P3 | 🔴 | L | — |
| — | Driver rating, shared leaderboards, more screens, public server list (N8), Windows on Arm | — | Parked | — | — | — |

## 2. Recommended order

1. **H1** in one sitting, with B8's cooling and S1's crown on the wheel.
2. **Visual quality pass.** G4 and N10, checked together on the RTX machine;
   C5 alongside, its S/F column first, the rest column by column as the
   kit gaps are built. The start/finish packages from the COTA comparison
   (C9 and C10, in any order, each judged on the `sf-ref-*`
   frames of several circuits)
   belong to this pass: they are independent of each other, and one or
   two at a time is the right pace, since the content ones re-dress
   and re-export every track.
3. **Sittings as people and hardware are there.** H2, V2 (R1/R2's tyre
   close-ups are framed with photo mode over the run's replay), P3 on an integrated-GPU machine.
4. Everything P3 opportunistically, bundled with whatever touches the same files (AU1's targets one at a time by ear).

## 3. Items by type

### Bugs

#### B15. Rim shakes violently at a standstill — P1 · 🔴 · S
*Source: follow-ups #31, wheel sitting 2026-10-23.* Standing still in the
GT3 at Monza (off the road after a crash) the rim shook hard. The server's
signals are clean there; the suspects left are the standstill damper and
the centring re-arming; the centring was made safe blind (no re-arm on a
stale flicker, gives up against hands; follow-ups #31). Needs the wheel: stop for 10 s, read the `Wheel
forces over 5 s standing:` lines, then tune the one they name. Run files:
`out/sit/2026-10-23/a-monza-1/`.

### Platform and stability

#### P3. Integrated GPU — P2 · 🔴 · S
Needs an Iris Xe / Arc or Radeon 680M/780M machine to confirm the LOW preset
and `GRHIDeviceIsIntegrated`. Until then keep "discrete GPU recommended".

### AI

#### A7. Imported F1s' AI touches other cars — P3 · 🟢 · S
*Source: follow-ups #19.* The imported F1s touch other cars 2.2-2.4
times as much as the shipped F1 (seeds 1-3, every circuit). Found: it is
side-by-side contact under the traffic dodge, from the cars' handling in
the AI's hands (the `[physics]` table's mass, size and centre of
gravity together, none alone), not brakes, tyres, aero, engine or dirty
air; a wider dodge cost the shipped classes contact and off-road. Left:
a fix, starting with the steering's recovery to the line (follow-ups
#19 has the numbers and what was tried).

#### A4. AI depth — P3 · 🟢 · M each
Bundle by module, each judged by the survey:

- **Racecraft:** switchback/cutback on exit; defence squeeze toward the
  edge; reading a player's intent (dummy, pulling out of the tow).
- **Strategy:** read rivals' stop windows and tyres; rejoin judged at the
  lane, not when decided.
- **Hybrid:** save the battery for a straight; read lap budget into where to
  attack.
- **Straight-line cap:** the skill cap costs 0.8 s/lap at Monza on
  hypercars; flat-out-on-straights raised off-road time 18% — needs a cap
  that only applies where the plan is optimistic.
- **Wet line:** a separate racing line and plan from the road-state bins
  (High effort, Low impact — do last).
- **Mistakes and fatigue:** self-made spins, cockpit heat, driver swaps.

Tried 2026-10-24 and kept out (16 circuits x seeds 1-3, three classes,
against contact 170.4, off-road 21.8, passes 1442): the switchback
(contact 186, passes 1438), giving room to a car that gives it (203,
off 7.3, 1517), a narrower beside-margin (211-233, passes ~1650), a
quicker car starting an attack 1.5-2.5 m/s faster (169-192 contact but
off-road 51-61), a level-with-the-car ahead rule (240), and tuck, abort
cool-down, plan share and late-braking tweaks (within noise). More passes
cost contact or off-road every time; the next try needs a pass the car
ahead sees coming, not a looser attack.

### Race rules and sessions

#### R4. Small race and qualifying rules — P3 · 🟢 · S–M each
- Driver swaps (fixed stint lengths are built).

### Simulation

#### S2. Low-impact physics gaps — P3 · 🟢 · M–L
Roll-centre migration and jacking; kerb steps as vertical body
acceleration (unsprung mass); porpoising with real heave. All Low impact —
only if a car class needs them.

#### Racing-line aid (player) — P3 · 🟢 · M
Replanned only at lap end, in still air, on one grip share for the lap.
Fold wind/wake and per-stretch grip in when the wet line (A4) exists.

### Rendering and visuals

#### G4. Road and terrain outside Lumen's distance-field scene — P3 · 🟡 · M
*Source: follow-ups #12.* The A/B is done (2026-10-24, `g4-df-fast` /
`g4-df-full`): giving every track mesh a distance field with the editor's
full build changes nothing on screen, because the flat road and ground
still stay out of the distance-field scene, so baked levels would not help.
Left: the terrain as a heightfield representation, or hardware ray-traced
Lumen, judged against the `g4-df-fast` sheet. Lowered to P3: no visible
fault found in the shots.

#### G6. Top view of formula cars looks cheap — P3 · 🟡
Looked at 2026-10-24 (`g6-top-view`, `g6-grid-above`; follow-ups, Top
view): one LOD, materials and shadows fine; from above at midday a formula
car's shadow hides under its floor. Needs the view named by the person who
saw it, then a contact shadow under the car.

### Content and circuits

#### C2. Circuit dressing backlog — P3 · 🟡/🔴 · M–L
*Source: follow-ups #6.*
- 🟡 Marina Bay: the F1 pit building, a quay rail, catch fence.
- 🟡 Stadium grandstands: bays and cap in `LayoutGrandstand`, the stair tower.
- 🟡 Nordschleife toll lanes' access lanes and car park.
- 🔴 Nordschleife rock cuttings: needs bare-earth DGM1 data or a surveyed list,
  plus a rock-face kit piece.
- 🔴 Nordschleife real marshal-post numbers: needs a source list.
- 🔴 Nordschleife OSM refit: the original extract is gone; a refit changes
  walls → groom + AI survey.

#### C3. Content tooling — P3 · 🟢 · S each
*Source: follow-ups #11.*
- Bare-earth check for Shanghai/Monza (needs bare-earth data).
- Silent `-ApexWatch` exit (not reproduced): watch for it; `-abslog` and the
  Windows event log if it returns.

#### C5. Circuit dressing to the references — P2 · 🟡 · L
*Source: roadmap 29; absorbs C2's dressing rows where a column reaches them.*
**What:** Every circuit brought up to its real venue's look from the
dossiers in `apexsim-content/docs/circuit-references/`, by column over all
tracks: S/F, landmarks, stands, trees, barriers, boards, people last.
**Implementation:** The audit is done: the status table, 13 kit gaps and
17 rule gaps are in
[track-improvements.md](../../apexsim-content/docs/track-improvements.md),
each track's positions from the maps in
`apexsim-content/docs/circuit-references/audit/<Stem>.md`
(`render_maps.py` renders the PDF maps). Every column is done
to its judged shots (2026-10-21 for S/F, 2026-10-22 for the rest) but for
the cells left open in track-improvements.md's status table, each with its
reason in the column's paragraph; the `c5-*` runs in
`scripts/verify/runs.yml` shoot any circuit again after a later edit.
The 2026-10-22 kit pieces (R21, R8, `c5-kit-*`, `c5-24h-*`) wait for a
person to judge the shots. Left: the Nordschleife's T13 pumps and media tower (no source places them); the four
circuits without a dossier (their OSM fits fail on layout variants:
`BBOXES`' notes); one track with selectable routes (follow-ups).
**Watch out:** hand-placed props are discarded on the next dress; the
barrier column changes `walls.msgpack` and needs the AI survey; a dress or
rule change touches every track's `.ats`, export and catalogue; people is a
new kit family plus client instancing and waits on nothing.

#### C9 and C10. COTA start/finish, the content packages — P2 · 🟡 · S–M each
*Source: roadmap 43 and 44, [cota-startfinish-comparison.md](cota-startfinish-comparison.md).*
Each stands alone and is judged on the `sf-ref-*` frames; none needs another first.
The track's audit page is
[Armadillos.md](../../apexsim-content/docs/circuit-references/audit/Armadillos.md).

- **C9. The stadium stand's canopy and rail** (roadmap 43): the crowd
  twin and setback are done; a tensile canopy and flags on the rail. S.
- **C10. Horizon for open-country circuits** (roadmap 44): a
  `forest_impostor` belt at the property edge for the prairie style, the
  DEM horizon mesh checked for the hills north of COTA. M.

**Watch out:** C10 re-dresses `--all` and touches every
track's `.ats` and export.

### Audio and features

#### AU1. Audio overhaul — P3 · 🔴 · L
*Source: follow-ups P4 (your note).* Scoped 2026-10-18 to four targets,
each its own package with a check: tyre squeal by each tyre's slip and
load, per surface; other cars' engines placed in 3D (distance, doppler,
exhaust direction); environment and reverb (crowd, wind, walls, stands,
tunnels); surface layers (gravel, grass, kerb rumble, wet spray). Every
result has to be judged by ear.
See `game/audio.md` — the sound is synthesised, so "updating assets" may not
apply.

#### F1. Windshield wipers — P3 · 🔴 · M
*Source: follow-ups P4.* Wiper mesh per cockpit (art), a key binding, an
animation and sound in first person, rain-driven auto mode. Server need: none
beyond the weather already on the wire. 🔴 for the models.

### Verification

#### V2. R1, R2, R7 — P2 · 🟡 · S each
- R1 Spa heavy rain: rain light, LEAK read-out, two flat spots (photo mode
  over the run's replay holds the car for the close-up).
- R2 Silverstone long stint: marbles and rubber, tyre wear visuals (photo mode),
  brake material colours and disc wear/stint energy (needs a custom car.toml
  with `disc_wear_per_mj` and `stint_energy_mj`).
- R7 seeded pile-up replay: dents, hanging parts, sparks, racecraft and
  reversing out of a barrier. Also the human look at side-by-side traffic
  since the 2026-10-24 traffic change (AI).

#### V3. R4, R5, R10 — P3 · 🟡 · S each
Snow/ice/melt and wind veer (R4 needs A2), Marina Bay night clouds and kerbs
(R5, pairs with G2), sun at solar noon and changeable sky (R10).

#### H1. Wheel, pedals and FFB sitting — P1 · 🔴 · S
*Source: verification §2.* The core of a sim, and none of it has been felt:
clutch, H-shifter, handbrake, front-axle hybrid, peak torques and minimum
forces, PXN profiles, kerbs, greasy tyres and damage, tread spread, banking
and crests, gravel and sand, flat spots, puddles, slow punctures. One
sitting, Nordschleife and Mandarina Bay. Every finding becomes a follow-up.
Planned block by block, with the agent's prep and follow-up, in
[wheel-sitting.md](wheel-sitting.md) (also covers H2's wheel rows).

#### H2. Other human sittings — P2 · 🔴 · S each
- **Racing the AI** (Monza, Zandvoort): room left alongside, defending.
- **Listen:** replay road/kerb sound, ghost, overheated squeal, engine
  knock, flat-spot thump, promo clip levels.
- **Pad and mouse:** browser filter pills, HUD editor R3/L3, hotlap
  transport, grid card focus, GameInput pad.
- **Imported pit stop** by hand.
- **Launcher AC import with the network off** — last.

### New features (also in the roadmap)

Not in any backlog before this plan; each is a package in
[roadmap.md](roadmap.md).

#### N8. Public server list (master server) — Parked
**What:** Players can only join a host they already know.
**Outcome:** Discoverable online racing.
**Implementation:** A small lobby service: servers register and heartbeat
(name, track, cars, slots, ping endpoint, TLS fingerprint); the session
browser gets an Internet tab. Opt-in per server in `operations` config.
**Parked 2026-10-18:** not now; players join known hosts by address.
**Watch out (when it returns):** hosting, domain and abuse handling are yours; servers
register themselves, so sign or verify them (TLS fingerprint); the browser's
filter pills already exist.

#### N10. DLSS / FSR — P3 · 🔴 · M
Render scale (TSR) and HDR are built. Left: DLSS and FSR plugins (decided 2026-10-18: ship both, TSR stays
the default; you accept NVIDIA's DLSS SDK terms when the plugin is
downloaded, so the download is yours), and the checks in verification.md.

#### N29. Broadcast HUD layout — P3 · 🟡 · S
*Source: roadmap 50.* A shipped layout of timing tower, lower third and
sector bar from HUD components, bindable to watching. Optional; last of
the COTA set.

#### N22. Localisation — P3 · 🔴 · M
Move UI strings to string tables; translations need translators (🔴).

#### N23. VR (OpenXR) — P3 · 🔴 · L
Cockpit view in a headset. Heavy on frame time (two eyes at 90 Hz), the HUD
must become world-space panels, mirrors cost extra, menus need a VR
pointer. 🔴: needs a headset and someone to sit in it; do after N10.

## 4. Needs a human — the short list

What only you can do, roughly in the order it pays off:

1. **Decided 2026-10-18:** D1 real layout, R3 a key per install, N7 an
   offline season first, N8 parked, B9 and B10, N10 DLSS and FSR, N14 offline TTS, AU1's
   four targets, UDP encryption closed; 2026-10-20: S1 in the refresh,
   X2 tests declare their backend, N28 before R3/N7.
2. **Sit at the wheel** — H1, then H2 (racing the AI, listen, pad), and
   client prediction over real latency ([verification.md](verification.md)).
3. **Hardware integrations** — a motion rig / SimHub / wheel LEDs for N2,
   a VR headset for N23.
4. **Leave the Windows/GPU box running** for the P1 crash-dump loop and the
   unattended verification runs (V2, V3).
5. **Confirm the agent's judgement** on the 🟡 items' screenshots.
6. **Source data, art or people** — DGM1 terrain and marshal-post list
   (Nordschleife), wiper models, integrated-GPU hardware, translators
   (N22).
7. **Scope** the parked ideas.

## 5. Parked / recommended to drop

- **Driver / safety rating** — depends on the proved driver ids (built) and R2; the local profiles' stats (N15) are built.
- **Shared leaderboards** — depends on the proved driver ids plus a service and trust model.
- **More screens** (five-wide, telemetry monitor) — no demand signal yet.
- **Public server list (N8)** — parked 2026-10-18; players join known hosts.
- **Windows on Arm** — parked until x64 emulation or a native build is worth it.

## 6. Notes on the backlogs themselves

- **roadmap.md is empty** apart from *Not yet scoped*; everything active now
  lives in follow-ups and simulation-gaps. Several sim gaps (flags,
  penalties, race events) are really product features and would sit better
  as roadmap packages with a check.
- **follow-ups P4 ("Unrefined")** holds your own notes without packages or
  checks (wipers, sound, top view, pit lane); this plan scopes them as B1,
  D1, F1, AU1 and G6.
- **New features N1–N23** were not in any backlog; they are now packages in
  `roadmap.md`. Yellow and blue flags stay in `simulation-gaps.md` (race
  rules) and are split here into R1a (flags, P1) and R1b (safety car, P2).
- **Verification:** R1/R2's tyre close-ups are framed with photo mode (N20)
  over a paused replay of the run.
