# Work plan

A prioritised view over the four backlogs, snapshot of 2026-10-16 at
`8dc654b`. The backlogs stay the source of truth for *what* is open
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

| ID | Item | Type | Pri | Human | Effort |
|---|---|---|---|---|---|
| V1 | Verify the newest session screens (R9, R11, R12) | Verification | P1 | 🟡 | S |
| H1 | Wheel, pedals and FFB sitting | Verification | P1 | 🔴 | S |
| B6 | Create-session screen too cluttered; shows a dead Sandbox mode | Bug | P2 | 🟡 | S |
| F2 | Trackside light panels showing yellow and blue | Feature + content | P2 | 🟡 | M |
| G4 | Runtime track meshes lack distance fields (A/B) | Rendering | P2 | 🟡 | M |
| D1 | Pit lane entry/exit layout and markings | Design + content | P2 | 🔴 | M |
| R3 | Race events across days (persistence, identity, schedule) | Race rules (epic) | P2 | 🔴 | L |
| S1 | Road crown and bumps | Simulation | P2 | 🟡 | M |
| A3 | Side by side past the apex on narrow roads | AI | P2 | 🟡 | L |
| V2 | Verification runs R1, R2, R7 | Verification | P2 | 🟡 | S each |
| H2 | Racing the AI, listening, pad sittings | Verification | P2 | 🔴 | S each |
| P3 | Integrated-GPU run | Platform | P2 | 🔴 | S |
| N5 | Multi-class racing | Race rules + AI | P2 | 🟡 | L |
| N6 | Lap comparison against the ghost (any two laps, MoTeC export built) | Driver tools | P3 | 🟢 | S |
| N7 | Championships and series with points | Game mode | P2 | 🔴 | M |
| N8 | Public server list (master server) | Online | P2 | 🔴 | M |
| N10 | DLSS / FSR upscaling, HDR paper white | Rendering | P3 | 🔴 | M |
| N11 | Livery editor / own livery import | Content | P2 | 🟡 | M |
| B5 | Chase camera inside a car body | Bug | P3 | 🟡 | S |
| G6 | Top view of formula cars looks cheap | Rendering | P3 | 🟡 | M |
| F1 | Windshield wipers | Feature | P3 | 🔴 | M |
| AU1 | Audio overhaul | Audio | P3 | 🔴 | L |
| C2 | Circuit dressing backlog | Content | P3 | 🟡/🔴 | M–L |
| C3 | Content tooling backlog | Tooling | P3 | 🟢 | S each |
| C4 | AC import fidelity gaps | Content | P3 | 🟢 | M |
| S2 | Low-impact physics gaps | Simulation | P3 | 🟢 | M–L |
| S3 | Environment: climate, dew, fog, snow detail | Simulation | P3 | 🟢 | M |
| A4 | AI depth: racecraft, strategy, hybrid, wet line | AI | P3 | 🟢 | M each |
| A5 | AI winter tyres (snow technique built) | AI + sim | P3 | 🟢 | M |
| R4 | Small race and qualifying rules | Race rules | P3 | 🟢 | S–M each |
| V3 | Verification runs R4, R5, R6, R8, R10 | Verification | P3 | 🟡 | S each |
| X1 | `SessionKind::Demo` removal | Cleanup | P3 | 🟢 | S |
| N13 | Rewind / restart from a point (offline) | Driver tools | P3 | 🟢 | M |
| N14 | Spotter and engineer radio | Audio | P3 | 🔴 | M |
| N15 | Driver profile and stats | Progression | P3 | 🟢 | M |
| N16 | Driving school / licence tests | Game mode | P3 | 🟡 | M |
| N17 | League rules: success ballast (reverse grid, BoP built) | Race rules | P3 | 🟢 | S each |
| N19 | Incident review for stewards | Online | P3 | 🟡 | M |
| N20 | Photo mode | Rendering | P3 | 🟡 | M |
| N22 | Localisation and colour-blind-safe HUD | Accessibility | P3 | 🔴 | M |
| N23 | VR (OpenXR) | Platform | P3 | 🔴 | L |
| — | Driver rating, shared leaderboards, more screens, UDP encryption, Windows on Arm | — | Parked | — | — |

## 2. Recommended order

1. **Verify what was just built** — V1: what is left of R9, R11 and R12 (two clients, a watcher's results screen, the lap allowance's garage), then H1 in one sitting. Fresh features are where the bugs are.
2. **Visual quality pass** — G4 of rendering work (close-ups can now be held: `apexsim.replay.Hold`). N10's render scale is built; check it with them.
3. **Design decisions you owe** — D1 (pit lane layout), R3's identity model, N7 (championship format), N8 (who hosts the server list). None can start without you.
4. **Online and depth** — N5 (multi-class), N11 (liveries), then R3 and N7 on top of identity.
5. Everything P3 opportunistically, bundled with whatever touches the same files.

## 3. Items by type

### Bugs

#### B5. Chase camera inside a car body — P3 · 🟡 · S
*Source: follow-ups #12.* Seen once on the Bugotti Chiffon. Check the chase
ladder's offsets against the car's bounds (`game/cameras.md`); reproduce
with `-ApexScreenshotAfter` on that car.

#### B6. Create-session screen too cluttered; shows a dead Sandbox mode — P2 · 🟡 · S
*Source: follow-ups #21 (testing, 2026-10-18).* The create-session screen is too crowded.
Give the race and conditions settings more room, up to half the screen.
The mode list still offers "Sandbox" (Free camera), which no longer exists:
drop it from the create screen's mode list
(`UI/ApexSessionCreateWidget.cpp`) and the lobby's
(`UI/ApexSessionLobbyWidget.cpp`); check whether `EApexGameMode` values are
saved or passed on the command line before removing the value. Check with `-ApexScreenshotAfter` on
the create screen; a person judges the new layout.

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

#### R3. Race events across days — P2 · 🔴 · L (epic)
*Source: simulation-gaps, race weekends.*

**What:** A weekend lives only as one in-memory session. No event entity, no
persistence across restarts, players identified per connection / by name, no
schedule, no AI identities, no combined results record.
**Outcome:** Real league-style weekends: practice Friday, qualifying
Saturday, race Sunday, same drivers, results kept.
**Implementation, in phases:**
1. *Stable driver identity* (account or token; car and livery fixed per
   event). Also fixes grid matching by name (`grid_order::resolve`) and is
   the base for leaderboards and ratings.
2. *Event entity + persistence* beside `records/` with deterministic ids.
3. *Schedule*: sessions open and start on time, lobby view and countdown,
   late joiners from the pit lane.
4. *AI entries* with fixed identities and levels across days.
5. *Weekend results record*, browsable replays and sheets.
**Watch out:** 🔴 because the identity model is a product decision (accounts?
tokens? trust?) — decide before building. Keep the sim path free of
persistence reads.

#### R4. Small race and qualifying rules — P3 · 🟢 · S–M each
- Qualifying tyre allowance and starting on the qualifying tyres.
- A qualifying HUD (not race standings).
- Setups and tyre sets carried between sessions (parc fermé).
- Assists per weekend phase — bundle with B2.
- Boxes dealt by team (`pit::deal_box`).
- Driver swaps / fixed stint lengths.
- Recovery vehicles and marshals (the safety car is built).

### Simulation

#### S1. Road crown and bumps — P2 · 🟡 · M
*Source: simulation-gaps.*

**What:** The road is flat edge to edge; no drainage camber, ripples or
seams; water runs only to a banked edge.
**Outcome:** More road feel through FFB, realistic puddling at the edges,
more convincing wet behaviour.
**Implementation:** Add a crown and a seeded bump field to the road
cross-section in `ue_export road` *and* to the centerline backend's surface;
let `road_state::edge_low` follow the crown.
**Watch out:** must land on both backends at once (the survey drives the
centerline, the live server the mesh); re-export tracks; AI survey before
and after; this changes every track's checksum, so showcases and guides go
stale.

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

#### D1. Pit lane entry/exit layout — P2 · 🔴 · M
*Source: follow-ups P4 (your note).*

**What:** Entry should be closer to the boxes and marked with a line; the
exit light should sit right after the last box, and control (no limit)
should return there.
**Outcome:** Pit lanes look and behave like real ones; shorter autopilot.
**Implementation:** Lines and the light come from the pit sidecar
(`<Stem>.pit.msgpack`) via the track pipeline; move `limit_end_m` / the exit
line to just past the last box and draw entry/exit lines on the road mesh.
**Watch out:** this needs a decision — real circuits put the limit line well
after the last box and the light at the lane's end, so "exit right after the
last box" is a gameplay choice, not realism. Regenerate sidecars (never edit),
`pit_stop_test` and `ai_pit_survey`, imports keep their AC lines.

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

#### C4. AC import fidelity — P3 · 🟢 · M
Altitude, north and geotags for imported tracks (Med impact: sea-level air,
wrong sun); AC's tyre heat model; dynamic aero controllers and DRS split;
two ERS motors and delivery profiles; brake pads/porpoising/compound details.

### Audio and features

#### AU1. Audio overhaul — P3 · 🔴 · L
*Source: follow-ups P4 (your note).* Spatial audio, engine realism,
environment, sliding/squeal and surface sound. **Needs scoping first:**
pick 2–3 concrete targets (e.g. tyre squeal by slip, exterior engine
spatialisation) with a check each. Every result has to be judged by ear.
See `game/audio.md` — the sound is synthesised, so "updating assets" may not
apply.

#### F2. Trackside light panels showing yellow and blue — P2 · 🟡 · M
*Source: follow-ups #18.* Race control's yellows and blues only reach the
HUD. Put marshal light panels on every circuit (`board/light_panel` is in
the kit, but no `.ats` places one yet: `ats-dress` lays them at the marshal
posts, at least one per 250 m sector) and drive them from the flags: yellow
flashing for the panel's sector, blue as a car being shown the blue passes.
Each panel needs its own dynamic glow instance (the builder shares one per
slot). Re-dressing changes the sidecars: re-export the tracks. Check with a
seeded pile-up (R7) and a lapping race on screen; a person judges.

#### F1. Windshield wipers — P3 · 🔴 · M
*Source: follow-ups P4.* Wiper mesh per cockpit (art), a key binding, an
animation and sound in first person, rain-driven auto mode. Server need: none
beyond the weather already on the wire. 🔴 for the models.

### Verification

#### V1. Newest session screens (R9, R11, R12) — P1 · 🟡 · S
Seen on 2026-10-17: the grid card with one client, DNQ in the standings and
results, PEN in the standings, the pit plan sent after a rejoin, the empty
grid slot in the card and (after a fix) the lobby. Left, per
verification.md: two clients (the guest's grid card, a late weekend entry)
which the runner cannot start yet; the tower's DNQ and PEN and the session
results screen (a watcher's view); the stop after a rejoin and the lap
allowance's garage (recipes updated, not yet run); weekend day skies.
`run_checks.py` rewrites `ApexProfile.sav`: keep an untouched copy.

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

#### X1. Remove `SessionKind::Demo` — P3 · 🟢 · S
Only once showcases exist everywhere; the backdrop still falls back to it
with stale or missing showcases, and `HotlapWatch` shares its create path.
Wire: remove only by leaving the variant decodable (append-only rule).

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

#### N7. Championships and series with points — P2 · 🔴 · M
**What:** No points table across races; R3 covers one weekend only.
**Outcome:** Offline career-like play and league seasons.
**Implementation:** A championship definition (rounds = track + rules,
points system, drop rounds), standings for drivers and teams, offline first
(saved in the profile), online on top of R3's persistence and identity.
**Watch out:** 🔴 for the design (points systems, tie-breaks, what an offline
season looks like); online needs R3 phase 1.

#### N8. Public server list (master server) — P2 · 🔴 · M
**What:** Players can only join a host they already know.
**Outcome:** Discoverable online racing.
**Implementation:** A small lobby service: servers register and heartbeat
(name, track, cars, slots, ping endpoint, TLS fingerprint); the session
browser gets an Internet tab. Opt-in per server in `operations` config.
**Watch out:** 🔴 hosting, domain and abuse handling are yours; servers
register themselves, so sign or verify them (TLS fingerprint); the browser's
filter pills already exist.

#### N10. DLSS / FSR, HDR paper white — P3 · 🔴 · M
Built: the Graphics tab's render scale (TSR; LOW 70, MEDIUM 85, HIGH and
ULTRA 100) and an HDR switch where the display supports it. Left: DLSS and
FSR plugins (🔴: their licences are a decision), a paper-white level for
HDR, and the checks in verification.md.

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

#### N13. Rewind / restart from a point (offline) — P3 · 🟢 · M
Offline practice only: rewind N seconds and resume. The sim is deterministic
and replays already store state; needs a periodic full-state snapshot of the
session. Watch out: laps after a rewind are invalid for records.

#### N14. Spotter and engineer radio — P3 · 🔴 · M
Voice cues: "car left/right/clear" (from N3), "yellow ahead", "blue flag"
(R1a), fuel and tyre calls, "box this lap" confirmation. The audio is
synthesised today, so this needs recorded lines or TTS: 🔴 for the voice.

#### N15. Driver profile and stats — P3 · 🟢 · M
Laps, distance, races, wins, PBs per track/car, incidents; a profile screen.
Offline from the save, online keyed on R3's identity. Base for a safety
rating later.

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

1. **Decide** D1 (pit exit: realism vs. gameplay), R3's identity model,
   N7's championship format, N8's hosting.
2. **Sit at the wheel** — H1, then H2 (racing the AI, listen, pad), and
   client prediction over real latency ([verification.md](verification.md)).
3. **Hardware integrations** — a motion rig / SimHub / wheel LEDs for N2,
   a VR headset for N23.
4. **Leave the Windows/GPU box running** for the P1 crash-dump loop and the
   unattended verification runs (V1–V3).
5. **Confirm the agent's judgement** on the 🟡 items' screenshots.
6. **Source data, art or people** — DGM1 terrain and marshal-post list
   (Nordschleife), wiper models, integrated-GPU hardware, radio voice lines
   (N14), translators (N22).
7. **Scope** AU1 (audio) and the parked ideas.

## 5. Parked / recommended to drop

- **Driver / safety rating** — depends on R3 phase 1 (identity), R2 and N15.
- **Shared leaderboards** — depends on R3 identity plus a service and trust model.
- **More screens** (five-wide, telemetry monitor) — no demand signal yet.
- **UDP encryption** — recommend closing: input is already sealed; telemetry
  carries nothing secret. Delete from the roadmap.
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
- **Blocked verification:** R1/R2 tyre close-ups wait on T2; R4 waits on A2;
  R12's last items wait on T1.
