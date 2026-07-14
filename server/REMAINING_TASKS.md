# ApexSim Server — Remaining Tasks

Status snapshot after the backend overhaul (2026-07-14). Phases 0–6 of the
plan are **done**; everything below is what's left. The full test suite is
green: 17 test binaries, `cargo fmt --check` clean, CI configured.

## What was completed (context)

- **Safety nets**: CI (fmt/clippy/build/test + Godot client build), Dependabot auto-merge now builds+tests first, in-process test harness (`server::run_server` on ephemeral ports — no manual server needed), criterion benchmarks, determinism test (bit-identical sim runs), proptest suite.
- **Security/ops**: token auth (`[auth]` config; dev mode explicit), TLS fail-closed default, per-connection rate limiting, pre-auth message gating, input clamping/NaN guards, config validation + `APEXSIM_*` env overrides, JSON file logging, Prometheus `/metrics`, SIGTERM/SIGINT graceful shutdown, per-session tick panic boundary, typed car/track loader errors + content validation.
- **Architecture**: `run_game_loop` monolith (760 lines) → `src/game_loop/{dispatch,tick,broadcast,lifecycle}`; lock-free message drain; telemetry serialized once per session (Bytes fan-out); LobbyManager single lock; unified disconnect path (100ms-sleep hack removed); tick rate plumbed (no hardcoded 240s).
- **Determinism**: `participants`/`ai_profiles` are BTreeMaps, deterministic default-car pick, no env/wall-clock reads in the sim path.
- **Physics correctness** (several found mid-overhaul by the race acceptance test):
  - vertical load double-count fixed (zero-sum suspension axle deviation),
  - roll stiffness includes springs (not ARBs only),
  - dt-scaled yaw damping,
  - yaw-aware OBB collision (SAT) with mass-weighted penetration separation,
  - Pacejka B-factor double-normalization fixed (launches ran at ~1/3 grip),
  - slip angles now include body lateral velocity (was "ice physics" — nothing resisted sliding),
  - lateral tire force sign fixed (car used to yaw against steering at speed and spin),
  - low-speed kinematic yaw blend (no more standstill pirouettes),
  - collision damage from closing speed (side-by-side contact no longer bricks cars),
  - off-track penalty no longer traps cars below recovery speed.
- **Performance**: windowed cached nearest-centerline search + adaptive spline density + O(log n) AI lookups. Hot loop ~185× faster: physics step 157µs → **0.8µs**, 8-car session tick 1.42ms → **7.4µs** (0.2% of the 4.17ms budget).
- **Race gameplay**: Race mode wired to `tick_racing`, countdown auto-transitions via `next_mode`, checkpoint-validated laps (with synthesized virtual checkpoints when tracks define none), DNF handling, finish positions. AI follows the raceline with curvature-based corner speeds, brakes for corners, uses all profile params, and completes full races on Monza unaided (`tests/race_flow_test.rs`).

## Remaining tasks

### 1. Phase 7 — Protocol evolution (client-coordinated; NOT started)
All of this breaks the Godot client's custom MessagePack serializer, so it must ship together with client changes, gated by a protocol version.

- [ ] Add `protocol_version: u8` to `Authenticate`; server rejects mismatches with a clear `AuthFailure`.
- [ ] **Wire up UDP** (user decision: implement per SPEC §3): UDP handshake message binding the client's UDP address to its `ConnectionId` (token from the TCP session); route `Telemetry` broadcast and `PlayerInput` receive over UDP (`transport.rs` `udp_receiver`/`udp_sender` exist and are spawned but unused); TCP stays for lobby/reliable messages.
- [ ] **Compact telemetry encoding**: telemetry only, `rmp_serde::to_vec_named` → positional `to_vec`; UUIDs → session-scoped `u8` car index (or 16-byte arrays). Lobby messages stay named. Expected ~40–60% byte savings.
- [ ] Configurable telemetry send-rate divisor (e.g. 60Hz snapshots of the 240Hz sim) — coordinate with client interpolation.
- [ ] Update the Godot client (`game-godot/scripts/csharp/`): UDP socket, handshake, compact telemetry decoding, protocol version.
- [ ] Verification: protocol-version rejection test, telemetry packet-size assertions, UDP loopback integration test in the in-process harness, joint manual test with the Godot client.

### 2. SPEC.md re-scope (user decision: reconcile with reality; NOT started)
- [ ] Rewrite §4.6 to describe the actual 4-wheel 3D model (spec still shows the 2D bicycle model).
- [ ] Mark §9 (race templates/rotation/scheduling), §10 (content hot-reload), §11 (SQLite persistence) as post-initial/deferred.
- [ ] Update §6/§7/§12 to "implemented" (rate limiting, JSON file logging, metrics) and §3/§1 once UDP lands.
- [ ] Remove or update the "Implementation status" banner added at the top of SPEC.md.

### 3. AI pace + racecraft tuning (functional but conservative)
- [ ] Current AI lap times at Monza: ~3:50–4:10 (real-world reference ~1:21). Raise the lateral-g budget/straight-line caps, tune braking points, and tighten `tests/race_flow_test.rs` lap-time upper bound (currently 320s) as pace improves.
- [ ] AI cars still take occasional off-track excursions (recovery works, but costs time) — better corner-entry speed planning would remove most of them.
- [ ] Tactical layer (overtaking, avoidance, racing side-by-side) is still missing — cars race the same line and rub; the closing-speed damage model tolerates it, but real avoidance is the fix.
- [ ] Qualification mode currently behaves as FreePractice (deliberate; see `game_session.rs` match on `GameMode`).

### 4. Physics refinements (deferred by plan)
- [ ] Per-wheel rotational state (real slip-ratio dynamics, wheelspin/lockup): the launch currently uses the `optimal_slip = 0.12` standstill hack, and engine RPM is slaved to wheel speed, so launches are engine-idle-limited (no clutch-slip model). This is the "Phase 4b" item.
- [ ] Combined-slip ellipse instead of `max(d_long, d_lat)` friction-circle clamp.
- [ ] Airborne/elevation jitter: cars occasionally flag airborne over crests and bounce loads; the z/elevation handling around `pos_z` snapping deserves a pass.
- [ ] Hybrid config (`[hybrid]` in car.toml) is parsed but unused by physics — wire it or remove it from the content packs.
- [ ] Grip retuning pass across the four content cars now that normal loads are physically correct (they lost the accidental ~2x grip; lap feel changed).

### 5. Smaller items
- [ ] Clippy warnings: ~65 remain (baseline was ~67). Burn down and switch CI to `-D warnings` (`.github/workflows/ci.yml` has the TODO).
- [ ] Replay metadata still uses placeholder `Player-{id}` names (`game_loop/tick.rs` TODO).
- [ ] `ServerHandle::shutdown` still holds the transport write lock across a 500ms sleep (harmless now that the loop doesn't contend, but ugly).
- [ ] Client `gear`/`clutch` inputs from `PlayerInput` are dropped server-side (always `None`) — wire them when the client sends them.
- [ ] `test_runner` TUI (`src/bin/test_runner.rs`) predates the in-process harness — update or retire it (`crossterm` is a full crate dependency only for this bin).
- [ ] Deferred spec features if ever needed: SQLite persistence (lap records/standings), race templates/rotation, content hot-reload.
- [ ] Consider committing this work in reviewable chunks — everything currently sits uncommitted in the working tree.

## Verification commands

```bash
cd server
cargo test                                   # full suite (17 binaries)
cargo test --test race_flow_test -- --nocapture   # AI race acceptance (~5s)
cargo test --test determinism_test           # bit-identical sim guard
cargo bench --bench physics_tick             # hot-loop benchmarks
cargo fmt --check && cargo clippy --all-targets
```
