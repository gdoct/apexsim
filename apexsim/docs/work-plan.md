# Work plan

A prioritised view over the four backlogs, snapshot of 2026-10-20 at
`1bbdb81`. The backlogs stay the source of truth for *what* is open
([roadmap.md](roadmap.md), [simulation-gaps.md](simulation-gaps.md),
[follow-ups.md](follow-ups.md), [verification.md](verification.md)); this
file says *in what order*, *what kind of work* and *who has to be there*.
When an item is done, delete it from its backlog and from here.

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
| A6 | AI lifts for trouble it sees ahead (tried 2026-10-20, not landed) | AI | P2 | 🟢 | M | 2 |
| N26 | Content packs and a content repository (phase 0 built) | Platform (epic) | P2 | 🔴 | L | 3 |
| R3 | Race events across days (identity, persistence, schedule) | Race rules (epic) | P2 | 🟡 | L | 3, 5 |
| N7 | Championships and series with points (offline first) | Game mode | P2 | 🟡 | M | 3, 5 |
| G4 | Runtime track meshes lack distance fields (A/B) | Rendering | P2 | 🟡 | M | 4 |
| C5 | Circuit dressing to the references (roadmap 29): audit done; kit gaps, per-column passes | Content | P2 | 🟡 | L | 4 |
| N5 | Multi-class racing | Race rules + AI | P2 | 🟡 | L | 5 |
| N11 | Livery editor / own livery import | Content | P2 | 🟡 | M | 5 |
| A3 | Side by side past the apex on narrow roads | AI | P2 | 🟡 | L | 5 |
| H2 | Racing the AI, listening, pad sittings | Verification | P2 | 🔴 | S each | 6 |
| V2 | Verification runs R1, R2, R7 | Verification | P2 | 🟡 | S each | 6 |
| P3 | Integrated-GPU run | Platform | P2 | 🔴 | S | 6 |
| R8 | Race control's edges: safety-car overlap, formation warmth, two-by-two rolling start (follow-ups #20) | Race rules | P3 | 🟢/🟡 | S each | 2 |
| N10 | DLSS / FSR upscaling, HDR paper white | Rendering | P3 | 🟡 | M | 4 |
| A7 | AI in imported cars leaves the road in traffic (follow-ups #19) | AI | P3 | 🟢 | M | — |
| N6 | Lap comparison against the ghost (any two laps, MoTeC export built) | Driver tools | P3 | 🟢 | S | — |
| B5 | Chase camera inside a car body | Bug | P3 | 🟡 | S | — |
| G6 | Top view of formula cars looks cheap | Rendering | P3 | 🟡 | M | — |
| C2 | Circuit dressing backlog | Content | P3 | 🟡/🔴 | M–L | — |
| C3 | Content tooling backlog | Tooling | P3 | 🟢 | S each | — |
| C4 | AC import fidelity gaps | Content | P3 | 🟢 | M | — |
| S2 | Low-impact physics gaps | Simulation | P3 | 🟢 | M–L | — |
| S3 | Environment: climate, dew, fog, snow detail | Simulation | P3 | 🟢 | M | — |
| A4 | AI depth: racecraft, strategy, hybrid, wet line | AI | P3 | 🟢 | M each | — |
| A5 | AI winter tyres (snow technique built) | AI + sim | P3 | 🟢 | M | — |
| R4 | Small race and qualifying rules | Race rules | P3 | 🟢 | S–M each | — |
| N17 | League rules: success ballast (reverse grid, BoP built) | Race rules | P3 | 🟢 | S each | — |
| N13 | Rewind / restart from a point (offline) | Driver tools | P3 | 🟢 | M | — |
| N15 | Driver profile and stats | Progression | P3 | 🟢 | M | — |
| N27 | Smaller recorded sessions (proposal) | Platform | P3 | 🟢/🟡 | M | — |
| N14 | Spotter and engineer radio | Audio | P3 | 🟡 | M | — |
| N16 | Driving school / licence tests | Game mode | P3 | 🟡 | M | — |
| N19 | Incident review for stewards | Online | P3 | 🟡 | M | — |
| N20 | Photo mode | Rendering | P3 | 🟡 | M | — |
| V3 | Verification runs R4, R5, R6, R8, R10 | Verification | P3 | 🟡 | S each | — |
| F1 | Windshield wipers | Feature | P3 | 🔴 | M | — |
| AU1 | Audio overhaul | Audio | P3 | 🔴 | L | — |
| N22 | Localisation and colour-blind-safe HUD | Accessibility | P3 | 🔴 | M | — |
| N23 | VR (OpenXR) | Platform | P3 | 🔴 | L | — |
| — | Driver rating, shared leaderboards, more screens, public server list (N8), Windows on Arm | — | Parked | — | — | — |

## 2. Recommended order

1. **H1** in one sitting, with B8's cooling and S1's crown on the wheel.
2. **Race control, one pass.** A6 again, from the Nordschleife's seed 2
   (follow-ups #20); R8's edges where they touch
   the same files.
3. **Built on the decisions of 2026-10-18.** N26 phase 1 (pack format,
   local install), R3 phase 1 (a key per install), N7 (an offline season).
   N26 phase 2 waits on the repository and the signing key.
4. **Visual quality pass.** G4 and N10, checked together on the RTX machine;
   C5 alongside, its S/F column first, the rest column by column as the
   kit gaps are built.
5. **Online and depth.** N5 (multi-class) with A3 (its speed differences
   stress side by side), N11 (liveries), then the rest of R3, and N7
   online on top of identity.
6. **Sittings as people and hardware are there.** H2, V2 (R1/R2's tyre
   close-ups wait on T2), P3 on an integrated-GPU machine.
7. Everything P3 opportunistically, bundled with whatever touches the same files (N27 with any `.apxs` work, AU1's targets one at a time by ear).

## 3. Items by type

### Bugs

#### B5. Chase camera inside a car body — P3 · 🟡 · S
*Source: follow-ups #12.* Seen once on the Bugotti Chiffon. Check the chase
ladder's offsets against the car's bounds (`game/cameras.md`); reproduce
with `-ApexScreenshotAfter` on that car.

### Platform and stability

#### P3. Integrated GPU — P2 · 🔴 · S
Needs an Iris Xe / Arc or Radeon 680M/780M machine to confirm the LOW preset
and `GRHIDeviceIsIntegrated`. Until then keep "discrete GPU recommended".

### AI

#### A3. Side by side past the apex on narrow roads — P2 · 🟡 · L
*Source: simulation-gaps, AI.*

**What:** After the apex, side-by-side cars fall back to the per-tick dodge,
which sweeps with the line; on Zandvoort two GT3s lean on each other.
Holding the dodge across the road was tried and made contact worse.
**Outcome:** Fewer side-by-side contacts; races on narrow circuits stay clean.
**Implementation:** A lane commitment through the exit (each car owns a
lateral band until one is a car length clear), built on `racecraft::pass_here`'s
lane choice rather than an offset on the line.
**Watch out:** the previous attempt failed — start with the survey's
contact count on Zandvoort as the target number; human review on R7.

#### A6. AI lifts for trouble it sees ahead — P2 · 🟢 · M
*Source: follow-ups #20.* Since a yellow waits `INCIDENT_CONFIRM_S`
(1.5 s), the AI no longer slows for a car only briefly in trouble ahead:
the whole-calendar survey (seeds 1-2) went from 485 to 654 car-seconds
off the road. An AI driver lifts for a car it sees off or crawling within
its view, apart from the flag rules. Judged by the survey against the
commit before; the target is back near 485. Tried 2026-10-20 and not
landed (the survey split by seed; the Nordschleife got worse): see
follow-ups #20 for the numbers and where to look next.

#### A7. AI in imported cars — P3 · 🟢 · M
*Source: follow-ups #19.* Imported F1s and prototypes finish their races
but leave the road two to four times as much as the shipped F1, in
traffic: find the lane or dodge their short lock cannot turn
(`SURVEY_EVENTS`, `SURVEY_TRACE`), judged by the survey on those cars,
seeds 1-3.

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

#### R3. Race events across days — P2 · 🟡 · L (epic)
*Source: simulation-gaps, race weekends.*

**What:** A weekend lives only as one in-memory session. No event entity, no
persistence across restarts, players identified per connection / by name, no
schedule, no AI identities, no combined results record.
**Outcome:** Real league-style weekends: practice Friday, qualifying
Saturday, race Sunday, same drivers, results kept.
**Implementation, in phases:**
1. *Stable driver identity*: a keypair made once per install; the server
   knows a driver by public key and a chosen name (decided 2026-10-18: no
   accounts, no service; a lost install is a new driver). Car and livery
   fixed per event. Also fixes grid matching by name (`grid_order::resolve`) and is
   the base for leaderboards and ratings.
2. *Event entity + persistence* beside `records/` with deterministic ids.
3. *Schedule*: sessions open and start on time, lobby view and countdown,
   late joiners from the pit lane.
4. *AI entries* with fixed identities and levels across days.
5. *Weekend results record*, browsable replays and sheets.
**Watch out:** the key signs a challenge at join and never leaves the
machine; keep it beside `ApexProfile.sav` and back it up with it. Keep the
sim path free of persistence reads.

#### R8. Race control's edges — P3 · 🟢/🟡 · S each
*Source: follow-ups #20* (A6 is its own item). The safety car drives
through a lapped car passing it; the formation lap puts no warmth in
(`FORMATION_LAP_WARMTH`); rolling starts run single file.

#### R4. Small race and qualifying rules — P3 · 🟢 · S–M each
- Qualifying tyre allowance and starting on the qualifying tyres.
- A qualifying HUD (not race standings).
- Setups and tyre sets carried between sessions (parc fermé).
- Assists per weekend phase — bundle with B2.
- Boxes dealt by team (`pit::deal_box`).
- Driver swaps / fixed stint lengths.
- Recovery vehicles and marshals (the safety car is built).

### Simulation

#### S2. Low-impact physics gaps — P3 · 🟢 · M–L
Roll-centre migration and jacking; kerb steps as vertical body
acceleration (unsprung mass); porpoising with real heave. All Low impact —
only if a car class needs them.

#### S3. Environment — P3 · 🟢 · M
Real time-zone boundaries and a better climate model (continental/maritime,
tropical seasons, altitude lapse); dew and fog after overnight cooling;
snow on run-off and in the pit lane with ice and a driven track; sleet and
hail.

#### Racing-line aid (player) — P3 · 🟢 · M
Replanned only at lap end, in still air, on one grip share for the lap.
Fold wind/wake and per-stretch grip in when the wet line (A4) exists.

### Rendering and visuals

#### G4. Runtime track meshes lack distance fields — P2 · 🟡 · M
*Source: follow-ups #12.* Software Lumen and DF shadows see road and terrain
poorly. Do the A/B: let the race director load `/Game/Tracks/<Track>/L_<Track>`
instead of building, then compare two screenshots. If it matters: ship baked
levels or use hardware RT Lumen.

#### G6. Top view of formula cars looks cheap — P3
Investigate LOD selection, shadow and material quality at that distance.

### Content and circuits

#### C2. Circuit dressing backlog — P3 · 🟡/🔴 · M–L
*Source: follow-ups #6.*
- 🟡 Marina Bay: corner names, the F1 pit building, a quay rail, catch fence.
- 🟡 Stadium grandstands: bays and cap in `LayoutGrandstand`, the stair tower.
- 🟡 Nordschleife bridges: an `Underpass` in `terrain.rs` for dossier roads
  (as at Suzuka), the road drawn under it, then the AI survey.
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
Built 2026-10-22 and waiting for a person to judge the shots: the real
pit lanes at Albert Park and Le Mans (R21, both pit surveys pass), the
pit-roof screens at Albert Park and Marina Bay, the tree rows at
Oschersleben and Shanghai (R8), Shanghai's lotus roof, the Fisht stadium,
Monza's banking (`c5-kit-*`), and the Nurburgring's 24h layout
(`Nuerburgerring24`, the GP and the Nordschleife joined by
`tools/combine_tracks.py`, GP pits; `c5-24h-nuerburgerring24`). Left: the
Nordschleife's T13 pumps and media tower (no source places them); the four
circuits without a dossier (their OSM fits fail on layout variants:
`BBOXES`' notes); one track with selectable routes (follow-ups).
**Watch out:** hand-placed props are discarded on the next dress; the
barrier column changes `walls.msgpack` and needs the AI survey; a dress or
rule change touches every track's `.ats`, export and catalogue; people is a
new kit family plus client instancing and waits on nothing.

#### C4. AC import fidelity — P3 · 🟢 · M
Altitude, north and geotags for imported tracks (Med impact: sea-level air,
wrong sun); AC's tyre heat model; dynamic aero controllers and DRS split;
two ERS motors and delivery profiles; brake pads/porpoising/compound details.

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
- R1 Spa heavy rain: rain light, LEAK read-out, two flat spots (needs T2).
- R2 Silverstone long stint: marbles and rubber, tyre wear visuals (T2),
  brake material colours and disc wear/stint energy (needs a custom car.toml
  with `disc_wear_per_mj` and `stint_energy_mj`).
- R7 seeded pile-up replay: dents, hanging parts, sparks, racecraft and
  reversing out of a barrier. Also the human look for A3.

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

### Cleanup

### New features (added 2026-10-16, also in the roadmap)

Not in any backlog before this plan; each is a package in
[roadmap.md](roadmap.md).

#### N5. Multi-class racing — P2 · 🟡 · L
**What:** The AI races in one class per session; no mixed fields (GT3 with
prototypes).
**Outcome:** Endurance racing as people know it, with traffic management as
its main skill.
**Implementation:** A field definition per class (cars, count, AI level) in
`CreateSession`; the timing tower and results by class (position in class,
class colours via `ApexCatalog::DisplayClass`); class-aware blue flags (R1a);
AI that yields to and passes across classes; grid by class.
**Watch out:** depends on R1a; the AI's speed difference between classes
stresses A3 (side-by-side); results and records keyed by class.

#### N6. Lap comparison: the ghost, any two laps, MoTeC — P3 · 🟢 · S
Built: every whole lap recorded and saved as CSV, any two held laps against
each other by distance in the pause menu (seen on 2026-10-18,
`n6-lap-traces`), and the MoTeC i2 export (`scripts/motec_export.py`, not
yet opened in i2: verification.md). Left: comparing against the ghost or
the hotlap record (the server's ghost traces are 20 Hz positions only).

#### N7. Championships and series with points — P2 · 🟡 · M
**What:** No points table across races; R3 covers one weekend only.
**Outcome:** Offline career-like play and league seasons.
**Implementation:** A championship definition (rounds = track + rules,
points system, drop rounds), standings for drivers and teams, offline first
(saved in the profile), online on top of R3's persistence and identity.
**Decided 2026-10-18:** an offline season against the AI first (calendar,
class, points preset, saved in the profile); online leagues later, on R3.
**Watch out:** online needs R3 phase 1; a person judges the season screens.

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

#### N10. DLSS / FSR, HDR paper white — P3 · 🟡 · M
Built: the Graphics tab's render scale (TSR; LOW 70, MEDIUM 85, HIGH and
ULTRA 100) and an HDR switch where the display supports it. Left: DLSS and
FSR plugins (decided 2026-10-18: ship both, TSR stays the default; you
accept NVIDIA's DLSS SDK terms when the plugin is downloaded), a
paper-white level for HDR, and the checks in verification.md.

#### N11. Livery editor / own livery import — P2 · 🟡 · M
**What:** Liveries come only from the catalog and AC skins.
**Outcome:** Personal cars; identity in online races.
**Implementation:** Phase 1: import a texture into
`content/cars/custom/<car>/liveries/` against the car's UV template (export
the template). Phase 2: an in-game editor (colours, decals, number, name).
Online: send the livery to other clients (size-capped, cached by hash).
**Watch out:** `content/cars/custom` is the player's and never ships; the
car's material comes from `ApexCarContent::OwnMaterialInstance`; no
trademarks on screen (decals are user content — say so).

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
`track-core` and the content tools go with the content. The roots are in
place (2026-10-19), with the content's own `bake.ps1`, a direction check
in CI, a release that ships the starter set (`starter.yml`) and the
formats crate `apexsim-lib/formats` owning the track YAML, sidecars and
`content_crc`, in a third root that the content and the code both build
against. Left: (optional) the sidecars baked out of the source folders. After that come the pack format and a local
install, the repository split (`git filter-repo --subdirectory-filter` per
root), the signed index with server and launcher updates, and
update-and-join.
**Decided 2026-10-19:** a pack states only the format levels it is
written in, never a code version; the code states the levels it reads.
**Decided 2026-10-18:** packs and the signed index are GitHub release
assets of `apexsim-content` (the index URL stays a setting); the starter set is one
car per class (GT3, LMP2, F1, Hypercar, Road) and three circuits (Spa,
Zandvoort, Monza).
**Human:** 🔴 still for creating `apexsim-content` and the signing key. The
split rewrites history and moves LFS, so it is done once, by hand, with
you there. What is left of phase 0 needs neither and is 🟢.
**Watch out:** the starter set has to carry a fresh install (the menu
showcases, the demo backdrop, the first-run defaults), but the tests run on
the whole content root.

#### N27. Smaller recorded sessions — P3 · 🟢/🟡 · M
*Source: roadmap #28, [proposals/replay-size.md](proposals/replay-size.md).*
**What:** A 16-car hour is 45 to 57 MB, stored on the server and on every
client in the session, and nothing ever deletes a server's recordings.
**Outcome:** Files about 40% of today's with nothing lost, and a cap on
what the recordings take.
**Implementation:** Columnar delta blocks under zlib (file only, version 2),
then ten-second blocks, then measured lossy cuts, then keep limits.
**Human:** 🟢 for phases 1, 2 and 4; 🟡 for phase 3, where the road sound
from coarser wheel slip has to be heard.
**Watch out:** the wire stays as it is (frames stay self-contained
datagrams); version 1 files must still play; `render` stays byte-identical
for a seed.

#### N13. Rewind / restart from a point (offline) — P3 · 🟢 · M
Offline practice only: rewind N seconds and resume. The sim is deterministic
and replays already store state; needs a periodic full-state snapshot of the
session. Watch out: laps after a rewind are invalid for records.

#### N14. Spotter and engineer radio — P3 · 🟡 · M
Voice cues: "car left/right/clear" (from N3), "yellow ahead", "blue flag"
(R1a), fuel and tyre calls, "box this lap" confirmation. Decided
2026-10-18: a fixed line list generated once with a local TTS model and
shipped as audio files. 🟡: the result is judged by ear.

#### N15. Driver profile and stats — P3 · 🟢 · M
Laps, distance, races, wins, PBs per track/car, incidents; a profile screen.
Offline from the save, online keyed on R3's identity. Base for a safety
rating later. Users should create a profile. The game should list availble profiles on launch. 
The profile selection screen has 16 AI generated driver portraits to choose from. These photos are in game-unreal/DriverImages but need to be moved elsewhere (they need to be in git).

#### N16. Driving school / licence tests — P3 · 🟡 · M
Short challenges built on the racing line, track guide and corner data:
brake at a board, take a corner under a target, a clean lap under time.
Medals unlock nothing at first. 🟡: difficulty needs play-testing.

#### N17. League rules: success ballast — P3 · 🟢 · S each
Built: the reverse top-N grid (`race_rules.reverse_grid`, server only like
the other race rules) and balance of performance per car as server.toml
`[[balance]]` tables (ballast, power, downforce; a server's call, as in
ACC's bop.json, so no wire). Left: ballast by championship position (needs
N7), and the create screen's rows (follow-ups #20).

#### N19. Incident review for stewards — P3 · 🟡 · M
A list of incidents (contacts, yellow-flag passes, track limits) with a jump
to that time in the replay, for admins in the dashboard or the client. After
R1a/R2.

#### N20. Photo mode — P3 · 🟡 · M
Pause a replay, free camera, focal length, depth of field, exposure, hide
HUD, save a high-resolution shot. Reuses the replay's cameras; needs the
replay pause from T2.

#### N22. Localisation and colour-blind-safe HUD — P3 · 🔴 · M
Move UI strings to string tables; translations need translators (🔴).
Colour-blind palettes first: wear, brake state, flags and deltas carry
meaning in colour alone today — add shapes or text and a palette switch.

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
   (Nordschleife), wiper models, integrated-GPU hardware, radio voice lines
   (N14), translators (N22).
7. **Scope** the parked ideas.

## 5. Parked / recommended to drop

- **Driver / safety rating** — depends on R3 phase 1 (identity), R2 and N15.
- **Shared leaderboards** — depends on R3 identity plus a service and trust model.
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
- **T1 is not tracked anywhere**: add it to follow-ups (content tooling).
- **Verification has no R3** — numbering gap, harmless.
- **New features N1–N23** were not in any backlog; they are now packages in
  `roadmap.md`. Yellow and blue flags stay in `simulation-gaps.md` (race
  rules) and are split here into R1a (flags, P1) and R1b (safety car, P2).
- **Blocked verification:** R1/R2 tyre close-ups wait on T2; R4 waits on A2.
