# Work plan

A prioritised view over the four backlogs, snapshot of 2026-10-24 at
`50d69b4e`. What the plan's finished items brought is in
[features-by-date.md](features-by-date.md). The backlogs stay the source of truth for *what* is open
([roadmap.md](roadmap.md), [simulation-gaps.md](simulation-gaps.md),
[follow-ups.md](follow-ups.md), [verification.md](verification.md)); this
file says *in what order*, *what kind of work* and *who has to be there*.
When an item is done, delete it from its backlog and from here.

## Current state (2026-10-24)

Main is green at `50d69b4e` (server tests pass but for an occasional
`test_disconnect_cleanup` timeout under load; 352 client tests). Every
agent branch of the 2026-10-23/24 night run is merged but one:

**A7/A4, half done, not merged.** Worktree
`E:pexsim\.claude\worktreesgent-a569ea4acbaf3b166`, branch
`worktree-agent-a569ea4acbaf3b166` (locked: `git worktree unlock` before
removing it). It holds:

- `ea4500d6` A4: a squeezed AI drops behind the car ahead, never against
  its own speed.
- `d77861df` A7: the imported prototypes' AI re-measured, on par with the
  shipped classes; the F1 imports are left to measure.
- Uncommitted edits in `server/sim/src/ai_driver.rs`, `hybrid.rs`,
  `lib.rs` and `racecraft.rs`, none of them judged by the survey yet; the
  agent was last looking at Shiverstone's off-road time.

To resume: in the worktree, build into a scratch `CARGO_TARGET_DIR`, run
`survey_ai_races_on_every_circuit` (seed 1, then 1-3 to confirm) and
compare totals with the baseline after A3 (main): **contact 104.1,
off-road 76.2, sliding 155.6, passes 565, 0 retired**. Keep what wins
passes back without raising contact or off-road; revert the rest. Then
measure the imported F1s (A7), look at the Snorisring hairpin at
480–500 m, cherry-pick onto main, rebuild the release server and
`scripts/build_physics_lib.ps1`, and delete A7/A4's finished rows here
and in follow-ups #19.

Next after that: what is left of N7 (step 4).

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
| B13 | Tent over the track at Armadillos T15–16 (follow-ups #6) | Bug | P2 | 🟡 | S | — |
| N26 | Content packs and a content repository (phases 0 and 1 built) | Platform (epic) | P2 | 🔴 | L | 2 |
| N7 | Championships: a fastest-lap point, per-round rules (offline season and online standings built) | Game mode | P2 | 🟡 | S–M | 4 |
| C5 | Circuit dressing to the references (roadmap 29): open cells and shots to judge | Content | P2 | 🟡 | L | 3 |
| H2 | Racing the AI, listening, pad sittings | Verification | P2 | 🔴 | S each | 5 |
| V2 | Verification runs R1, R2, R7 | Verification | P2 | 🟡 | S each | 5 |
| P3 | Integrated-GPU run | Platform | P2 | 🔴 | S | 5 |
| N10 | DLSS / FSR upscaling | Rendering | P3 | 🟡 | M | 3 |
| G4 | Road and terrain outside Lumen's distance-field scene (A/B done) | Rendering | P3 | 🟡 | M | 3 |
| A7 | Re-measure the imported F1s' AI in traffic (follow-ups #19) | AI | P3 | 🟢 | S | — |
| G6 | Top view of formula cars looks cheap | Rendering | P3 | 🟡 | M | — |
| C2 | Circuit dressing backlog | Content | P3 | 🟡/🔴 | M–L | — |
| C3 | Content tooling backlog | Tooling | P3 | 🟢 | S each | — |
| C4 | AC import fidelity gaps | Content | P3 | 🟢 | M | — |
| S2 | Low-impact physics gaps | Simulation | P3 | 🟢 | M–L | — |
| A4 | AI depth: racecraft, strategy, hybrid, wet line (in progress) | AI | P3 | 🟢 | M each | — |
| R4 | Small race and qualifying rules | Race rules | P3 | 🟢 | S–M each | — |
| V3 | Verification runs R4, R5, R6, R8, R10 | Verification | P3 | 🟡 | S each | — |
| F1 | Windshield wipers | Feature | P3 | 🔴 | M | — |
| AU1 | Audio overhaul | Audio | P3 | 🔴 | L | — |
| N22 | Localisation | Accessibility | P3 | 🔴 | M | — |
| N23 | VR (OpenXR) | Platform | P3 | 🔴 | L | — |
| — | Driver rating, shared leaderboards, more screens, public server list (N8), Windows on Arm | — | Parked | — | — | — |

## 2. Recommended order

1. **H1** in one sitting, with B8's cooling and S1's crown on the wheel.
2. **N26:** (optional) the sidecars baked out of the source folders;
   phase 2 waits on the repository and the signing key.
3. **Visual quality pass.** G4 and N10, checked together on the RTX machine;
   C5 alongside, its S/F column first, the rest column by column as the
   kit gaps are built.
4. **Online and depth.** What is left of N7 online (a fastest-lap point,
   per-round rules, standings in the lobby and the results).
5. **Sittings as people and hardware are there.** H2, V2 (R1/R2's tyre
   close-ups are framed with photo mode over the run's replay), P3 on an integrated-GPU machine.
6. Everything P3 opportunistically, bundled with whatever touches the same files (AU1's targets one at a time by ear).

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

#### B13. Tent over the track at Armadillos T15–16 — P2 · 🟡 · S
*Source: follow-ups #6, wheel sitting 2026-10-23.* Not found in the scene:
the only prop over the road there is the T16 footbridge, a truss gantry
with a PIRETTI banner (verify run `b13-armadillos-t15`). Needs the driver
to point at it again (a shot or a MARK where it stands).

### Platform and stability

#### P3. Integrated GPU — P2 · 🔴 · S
Needs an Iris Xe / Arc or Radeon 680M/780M machine to confirm the LOW preset
and `GRHIDeviceIsIntegrated`. Until then keep "discrete GPU recommended".

### AI

#### A7. Imported F1s' AI in traffic — P3 · 🟢 · S
*Source: follow-ups #19.* The imported prototypes now leave the road no
more than the shipped classes (the traffic dodge measured across the
road did it); the imported F1s were not installed where that was
measured: install them, run the survey on them (seeds 1-3) and, if they
still leave the road in traffic, find the lane or dodge their short
lock cannot turn.

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

### Race rules and sessions

#### R4. Small race and qualifying rules — P3 · 🟢 · S–M each
- Assists per weekend phase — bundle with B2.
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
- 🟡 Marina Bay: corner names, the F1 pit building, a quay rail, catch fence.
- 🟡 Stadium grandstands: bays and cap in `LayoutGrandstand`, the stair tower.
- 🟡 Nordschleife toll lanes' access lanes and car park.
- 🔴 Nordschleife rock cuttings: needs bare-earth DGM1 data or a surveyed list,
  plus a rock-face kit piece.
- 🔴 Nordschleife real marshal-post numbers: needs a source list.
- 🔴 Nordschleife OSM refit: the original extract is gone; a refit changes
  walls → groom + AI survey.
- 🟢 Hotlap: corners carry the official turn numbers from the dossier.

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

#### C4. AC import fidelity — P3 · 🟢 · M
Aero controllers on brake/throttle/steer and the DRS flap split; ERS
delivery profiles and the second motor's curve; a compound's own load
sensitivity (brake pads, `brake_by_wire` and porpoising have no AC figure).

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

#### V3. R4, R5, R6, R8, R10 — P3 · 🟡 · S each
Snow/ice/melt and wind veer (R4 needs A2), Marina Bay night clouds and kerbs
(R5, pairs with G2), Nordschleife dressing close-ups (R6), stadium stands
(R8), sun at solar noon and changeable sky (R10).

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

#### N7. Championships and series with points — P2 · 🟡 · M
The offline season and online championships are built
([features-by-date](features-by-date.md),
[sessions](server/sessions.md#championships)). **Left:** a fastest-lap point; per-round rules; the standings in the
lobby and on the results screen of a championship's event.

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

#### N26. Content packs and a content repository — P2 · 🔴 · L (epic)
*Source: roadmap #27, [proposals/split-off-content.md](proposals/split-off-content.md).*
**What:** One repository and one release zip hold the code and every car,
circuit and prop: a clone is a gigabyte of LFS, every fix ships every
track, and nothing states which content fits which code.
**Outcome:** Versioned content packs installed and updated apart from the
code, a release that copies a starter set from the content, and
a join that offers to update a mismatched track.
**Implementation:** Five phases, each with its own check in the proposal.
Phase 0 splits this repository into two roots, `apexsim/` and
`apexsim-content/`, laid out as the future repositories, with the code
depending on the content and never the reverse; the track editor,
`track-core` and the content tools go with the content. Phase 0 is built (the three roots). Left: (optional) the sidecars baked out of the source folders. After that come the pack format and a local
install, the repository split (`git filter-repo --subdirectory-filter` per
root), the signed index with server and launcher updates, and
update-and-join.
**Decided 2026-10-19:** a pack states only the format levels it is
written in, never a code version; the code states the levels it reads.
**Decided 2026-10-18:** packs and the signed index are GitHub release
assets of `apexsim-content` (the index URL stays a setting); the starter set is one
car per class (GT3, LMP2, F1, Hypercar, Road) and three circuits (Spa,
Zandvoort, Monza).
Phase 1 (the pack format and a local install,
[packs.md](../../apexsim-content/docs/packs.md)) is built and checked.
**Human:** 🔴 still for creating `apexsim-content` and the signing key. The
split rewrites history and moves LFS, so it is done once, by hand, with
you there. What is left of phase 0 needs neither and is 🟢.
**Watch out:** the starter set has to carry a fresh install (the menu
showcases, the demo backdrop, the first-run defaults), but the tests run on
the whole content root.


#### N22. Localisation — P3 · 🔴 · M
Move UI strings to string tables; translations need translators (🔴).

#### N23. VR (OpenXR) — P3 · 🔴 · L
Cockpit view in a headset. Heavy on frame time (two eyes at 90 Hz), the HUD
must become world-space panels, mirrors cost extra, menus need a VR
pointer. 🔴: needs a headset and someone to sit in it; do after N10.

## 4. Needs a human — the short list

What only you can do, roughly in the order it pays off:

1. **Decided 2026-10-18:** D1 real layout, R3 a key per install, N7 an
   offline season first, N8 parked, N26 on GitHub releases with a
   class-wide seed, B9 and B10, N10 DLSS and FSR, N14 offline TTS, AU1's
   four targets, UDP encryption closed; 2026-10-20: S1 in the refresh,
   X2 tests declare their backend, N28 before R3/N7. Left: create `apexsim-content` and
   its signing key (N26 phase 2).
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
