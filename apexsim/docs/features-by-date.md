# Features by date

What the work plan's items brought, newest first: each item's commits on the
day they landed (`git show <hash>` for the change). The open items are in
[work-plan.md](work-plan.md); what is left of a built feature is in its
backlog ([follow-ups.md](follow-ups.md), [simulation-gaps.md](simulation-gaps.md),
[verification.md](verification.md)). Add a date's section when items land.

## 2026-10-25

- **A7** — the imported F1s' contact found: side by side under the dodge, from their handling (`58c0eb48`)
- **Follow-ups #28**
  - no HUD panel drawn over another; gaps off each car's trail (`3abd877e`)
  - the conditions panel's lines wrap; one lap is no chart (`a1a45607`)
- **N7** — a season round's own rules on a line under its name (`49f4d4b0`)
- **Follow-ups #34** — the qualifying garage tells the new tyre sets left, or the used set going out (`72a1173f`)
- **Follow-ups #20** — stewards give a penalty from the dashboard (`1cda4e2a`)
- **Follow-ups #32 (fog)** — the fog's visibility in a stream's state record (`82dcf57e`)
- **R4 (verification)**
  - the stand-in decides as the AI does, so a stuck one is put back (`157921d4`)
  - a car is named from the roster as the recorder first sees it (`5365c462`)
  - an away driver's held car is put back when stuck (`8de6e2ff`)
- **Follow-ups #32** — the create screen's air in the circuit's climate, with its height (`8f5b3a0e`)
- **Follow-ups #35** — the rewind ring measured: 253 MB at the Nordschleife (`91277886`)
- **Follow-ups #99**
  - the track pipeline fmt and clippy clean again (`fbe91151`)
  - the protocol tests wait up to 60 s (`e1b02e65`)

## 2026-10-24

- **A3**
  - AI traffic dodge measured across the road; drop in behind a car alongside and ahead (`d4dc340f`)
  - contact and off-road halved, passes down a third; the lobby's event line ends in an ellipsis (`2debcb87`)
- **A5** — AI winter tyres read the forecast, studs pull out on tarmac (`c911e962`)
- **B13** — the observation tower's plaza canopy reaches away from the road (`a16ee954`)
- **C2** — the roads under the Nordschleife's bridges (`e2c0711a`)
- **C4**
  - a compound's own heating (`f28d9a56`)
  - wings that move with the speed (`68e61677`)
  - a second ERS motor at the crank (`f32ef7cc`)
  - a compound's own best pressure (`b416122e`)
  - wings that move with the driver (`872f52ee`)
  - a compound's own load sensitivity (`763a603c`)
  - an ERS delivery profile and the second motor's curve (`85c7a9c4`)
  - a delivery profile only takes deployment away (`9bed23ef`)
  - an imported car's DRS flap opens (`21b1caba`)
- **G4** — the distance-field A/B (-ApexTrackFullBuild): flat road and ground stay out of the DF scene (`170e0e5e`)
- **G6** — the formula cars seen from above, shot and written up (`6ebd5c31`)
- **N10** — an HDR paper-white level on the Graphics tab (`94307e6e`)
- **N11**
  - the player's own liveries and a livery template tool (`2edbe4c7`)
  - own liveries seen online, and a livery editor in the garage (`abff58eb`)
- **N13**
  - rewind in offline practice, the server and the wire (`3376e6ee`)
  - the rewind test's driver keeps its heartbeat up through the countdown (`65b514ba`)
  - rewind from the pause menu, the cars' buffers dropped on Rewound (`867a373c`)
- **N14** — spotter and engineer radio (`60d5573f`)
- **N16** — driving school, licence tests judged on the client (`85aa5585`)
- **N17** — success ballast by championship position in the offline season (`1d86ef55`)
- **N19**
  - incident review for stewards (`d1574ec7`)
  - the race's incident list on the results screen, and -ApexWatchReplayAt (`89cc9940`)
- **N20**
  - photo mode over a watched race: free and orbit camera, lens, high-resolution shot (`d51a7574`)
  - an orbit taken up from a wide shot starts 12 m from the car (`a48c9783`)
- **N22** — colour-vision palettes for the HUD, and no state in colour alone (`c5194503`)
- **N26**
  - the launcher strips the mark of the web from a release package (`aa69d926`)
  - the launcher unblocks only the binaries the release shipped (`ce17e33d`)
  - the launcher's content management in modules of its own (`1ecfb2a2`)
  - loose content packs (.acp): cars, tracks and HUD components from a zip (`f6953d7f`)
  - a release ships the content outside the starter set as content/content.acp (`2a696a09`)
  - pick which items of an .acp to install (`b784e83b`)
  - -NoContentPack builds the starter set alone (`64d48a12`)
  - manage content: tick your own items and uninstall them (`6b941096`)
- **N27**
  - column blocks, version 2 stream files (`e8ee00f3`)
  - ten-second column blocks (`d7b95787`)
  - replay keep limits; replay_size.py (`c8199104`)
- **N6** — the record lap among the lap traces, laid on the player's own line (`1a691571`)
- **N7**
  - championships over race events, standings by proved driver id (`accbe12b`)
  - the offline season's fastest-lap point and each round's own rules (`7a06b879`)
  - a fastest-lap rule per championship, the standings in a round's lobby and results (`d5130c89`)
- **R3**
  - race events kept across server restarts (`8f2ee7a2`)
  - an event's AI entries keep their identities and levels (`595cd155`)
  - race events run to a schedule (`10cba862`)
  - the weekend sheets as one record, exported and browsable (`f050f32e`)
- **R4**
  - a qualifying tyre allowance and the race started on the qualifying set (`3a72209b`)
  - parc fermé and a weekend's tyre allocation (`5d1dd815`)
  - pit boxes dealt by team (`2d1369ee`)
  - fixed stint lengths (`e7e8ba83`)
  - a qualifying HUD, the standings by best lap (`263cde83`)
  - marshals and a recovery tractor at a stopped car (`74ecbcff`)
  - practice and qualifying cap the aids of their own (`6ef2974c`)
- **S3**
  - dew on a still clear night and a radiation fog (`8ef91a9b`)
  - the packed pit lane and the half-melted run-off glaze on a cold night (`44e44c11`)
  - the environment item off the work plan (what is left is in the backlogs) (`6e8aea01`)
  - the height fog thickens to the server's visibility (`22f464bc`)

- **Other, 2026-10-24**
  - R3 in the game and on the dashboard: the session's event in `SessionJoined`, the lobby naming it, the dashboard's Events view (`94f26e3c`)
  - R4 on the client: the tyre, parc fermé and stint rules on the create screen, a stint HUD, held knobs in the garage (`85df5904`)
  - Replays: the client records version 2 and shows each replay's size (`ebae5e09`)
  - TV director: never films from inside a mesh (`70562e2d`)
  - Cockpit screens dim with the view's exposure at night (`b8b576a9`)
  - Headlights: a narrow beam aimed near level, no white disc at the nose (`9733c433`)
  - Main menu: the Esc card centred over its dim (`5fd88b7b`); `ApexUI::AlignContent` (`6aeddafd`)
  - Tests: the showcase and guide checks render their reference with the server's apexsim-replay (`963278da`)

## 2026-10-23

- **A6** — the early lift did not cut off-road time, and the loss it chased is gone (`76e22069`)
- **B12** — a flat spot is ground only once a lock-up holds (`104161fa`)
- **B13** — nothing but the footbridge over the road at Armadillos T15-T16 (`4f0f5e4a`)
- **B14** — the Wheel tab matches a base's range; a gentler direction test (`1ce7087f`)
- **B15**
  - a force line for a car standing still (`794852c9`)
  - the centring neither re-arms on a stale frame nor fights hands (`b183db91`)
  - the blind centring fix noted in the backlogs (`45ea0da7`)
- **B16**
  - downshift protection; the limiter cuts all torque; a retired car gets the finish view (`86b80955`)
  - the over-rev and refused downshift told on screen (`9206e4b2`)
- **B5** — the chase camera is kept out of the followed car's body (`56f9616d`)
- **C4**
  - an imported track's height, place and north (`92a64149`)
  - a compound's own window width (`34d1b005`)
- **N15** — driver profiles and stats (`4e332351`)
- **N26**
  - content packs and a local install (`48d577fe`)
  - packs installed into a release package and raced with no mismatch (`77ff6946`)
- **N5**
  - multi-class fields (`94fc5cbe`)
  - multi-class racing (`4c8a7c03`)
- **N7** — an offline season against the AI (`4f797314`)
- **R3** — driver keys prove a profile's id (`f388c40f`)
- **R8**
  - a rolling start forms up two by two (`a75bac4e`)
  - a car let by goes round the safety car (`cdc68ad3`)
  - a formation lap warms its grid's tyres itself (`46341266`)
- **S3** — a dated session's air in the circuit's climate (`5ff17848`)

## 2026-10-22

- **C5**
  - one Steintribune centre block, darker bank turf (`16d38f2b`)
  - the column's state (`becace99`)
  - the dossier pass (`84588b8b`)
  - re-dressed and baked, shot runs (`295b908f`)
  - the corner tables (`b55246ac`)
  - the column's state (`4711e4c5`)
  - the rules baked; the people laid (`d1ae2dd5`)
  - shot runs (`feb15510`)
  - the columns' state (`0ab5038b`)
  - shots judged; cells closed (`d8988362`)
  - Albert Park Lake shots in c5-lm-albertparkour (`c5a954eb`)
  - R14 and lake shots judged, their cells closed; day venues keep their floodlights (R15) (`df2a3b6c`)
  - the R13 cells closed (their stands entered by hand in the Stands pass); R13/R22 to follow-ups as extract rules (`710c646f`)
  - R21 real pit lanes, R8 tree rows, pit-roof screens, Grandstand A and Fisht placed (`3b1a1753`)
  - R21 lanes, R8 tree rows, roof screens, Grandstand A, Fisht, the Sopraelevate (`35f2c9e7`)
- **R12** — podium_m in the integration tests' pit lanes (`e2348a49`)
- **R14**
  - big buildings stepped back off the run-off, trimmed to their side of the road (`8309ae1a`)
  - the MSV Centre, the Salons de l'Eau Rouge, Lakeside Stadium and four more (`3087d47d`)
- **R17**
  - boards past a flat-out kink; a stand laid against another leg is reported (`15a5f16b`)
  - distance boards at 18 more corners; c5-r17-zandervoort shots (`94dbe00e`)

## 2026-10-21

- **C5**
  - the dressing audit of every dossier circuit against its references (`18e0ec99`)
  - a stand's roof by name, and stands beside the pit lane kept (`c4d38122`)
  - BrandsScratch's pit-straight stand roofs from the aerial (`b4b6716c`)
  - ChillyVilleneuve's Grandstand 1 and Platine from the maps, paddock screens (`d1f55caf`)
  - Gnocchi's T1 stand, Hockeyheimring's Nordtribuene and screen (`7632a90c`)
  - Shebang's main stand faces from OSM's outline, the Tower stand at the V (`c2070366`)
  - the pit straights of six circuits from their maps (`e25f90eb`)
  - the pit straights of seven more circuits (`eb32af9c`)
  - Snorisring, SpaFrankenchamps, Sudoku and Zandervoort's straights (`eafd8f17`)
  - MandarinaBay's Republic stand at T5, pit-exit stands behind the exit road, two superscreens (`b8025d88`)
  - every circuit re-dressed with its start/finish pass (`544e002c`)
  - the start/finish shots judged; the column's cells closed (`b75f5385`)
  - how a column is worked, and its two helper scripts (`0d40318b`)
  - the dossier pass (`59610595`)
  - re-dressed and baked, shot runs (`a20641b6`)
  - bridge shots from down the road; the column's state (`713599af`)
  - the dossier pass (`fa153995`)
  - re-dressed and baked; stands clear of the run-off (`e0cfb565`)
- **N28** — no build stage renders a showcase or a guide; the server renders its own (`03da0fe4`)
- **X1** — a server refuses demo and watched-hotlap sessions; the game makes its own (`9f335df9`)

## 2026-10-20

- **A6** — tried, not landed; the numbers and where to look next (`4d9680da`)
- **B11** — the tyre bridge is a flat arc on two low legs (`9e32ae0e`)
- **B7** — the road's half-widths are filtered; the curb kinks were their wobble (`e08a50aa`)
- **D1** — the pit speed limit starts 60 m before the first box (`b54dc40e`)
- **F2** — marshal light panels flash the yellows and blues round the circuit (`264136f5`)
- **N25** — a rain radar over the minimap (`cc76b488`)
- **N28**
  - the physics library runs a seeded race, the render's own (`ab73ba2a`)
  - the game renders its own showcases, the build's bytes (`f2074112`)
  - the game makes a missing or stale track guide when opened (`51301b11`)
  - Watch hotlap simulated in the game, server or not (`f075fb92`)
- **R6** — race control on screen: the safety car drawn, the phase on the dash, the rules in the lobby (`104f8707`)
- **R7** — race control's flags in the car, a cue and the dash's light bar (`b813e83b`)
- **R8**
  - the pace race control holds the car to, on the cockpit dash (`e9a73d6c`)
  - a predicting client steps its car balanced as the server does (`4778f090`)
- **S1** — the road has a crown and bumps, on both backends (`9e95c3c7`)
- **X2** — circuit tests name the sidecars they need (`69dcc082`)

## 2026-10-19

- **B10** — car and track select: a click selects, Confirm or a double-click chooses (`98459054`)
- **B6** — create screen settings take half the screen; Sandbox leaves the mode lists (`0b47c9c8`)
- **B9** — Watch a race is a page with Replays, Hotlap on Drive, Garage's row is Cars (`153282a4`)
- **N24** — a tyre graphic on the HUD, and item.flat_spot_pct (`5aeffc06`)
- **N26**
  - housekeeping before the content move (`9197de95`)
  - the repository holds two roots, apexsim/ and apexsim-content/ (`83df34f2`)
  - the content bakes itself (apexsim-content/bake.ps1) (`1be809fb`)
  - CI fails when the content reaches into the code (`5d223efe`)
  - a release ships the starter set (`54f62394`)
  - the formats crate apexsim-lib/formats (`0e015f26`)

## 2026-10-18

- **A5** — a snow technique: slower than the plan, a slower wheel and a squeezed throttle (`d36b94bb`)
- **B8** — an overheated tyre sheds it in about two gentle laps (`9b26d3b7`)
- **C2** — every camp site pitches its tents on the R2 sequence, not in a row (`f57511c7`)
- **C3**
  - stamp each surroundings sub-layer and named area; every shipped scene re-dressed for its stamps (`5d56f5aa`)
  - a circuit without a DEM sidecar takes the dossier's own OSM fit (Marina Bay) (`ce97eb49`)
  - Suzuka's centerline z from the DEM, the crossover held at 7 m (`530a7181`)
- **N17**
  - reverse grid race rule turns the front of the starting order round (`e32a979c`)
  - balance of performance per car from server.toml [[balance]] (`631e3cf8`)
- **N18** — results webhook posts each race result as JSON or a Discord message (`9ca6b405`)
- **N6**
  - motec_export.py turns lap trace CSVs into a MoTeC i2 .ld and .ldx (`4a6e94f7`)
  - LAP TRACES picks any two held laps; the recorder logs why it drops a lap (`672a78bf`)
- **R4**
  - without a pit lane a car the race outruns is topped up at the line (`1815d138`)
  - a qualifying car goes out from its box down the pit lane (`8def718b`)
- **R5** — a car is an incident only after 1.5 s in trouble (`854af21f`)

## 2026-10-17

Overnight run (commit subjects of that night do not carry item ids):
B1 pit hand-back, B2 a weekend's day sky, B3 AC import traps, T1 the
stand-in in qualifying, A2 AI in the snow (traction control cuts engine heat
and fuel), R1a yellow and blue flags (`race_control.rs`), N3 radar, C1 dress
drops buildings on paved run-off, P2 tick jitter log, W1 spectator parity, T2
replay hold and seek, N18 results export, N21 benchmark
(`scripts/benchmark.py`), G2 floodlit exposure ceiling; a GPU soak of 88 runs
clean.

## 2026-10-16

Two runs over [simulation-gaps.md](simulation-gaps.md) in parallel packages:
road state, car physics, conditions, off-track ground, AI strategy, AI
driving, session rules, AC import; then the racing-line aid refresh, pit plan,
pit rule and AI strategy, wind veer, seasons, lane snow, the host's forecast,
tyre zones and crests, AI back-out, mistakes, engine nursing and ERS on
straights, the qualifying shoot-out, lap allowance and per-phase damage, AC
front-axle motor, racecraft (pass place, defence, room for a player).
