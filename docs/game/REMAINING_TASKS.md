# ApexSim Server — Remaining Tasks

Status snapshot after the 2026-07-17 completion pass. Everything from the
previous list is **done** except the explicitly-deferred items below. The
full test suite is green (18 test binaries incl. `protocol_test.rs`),
`cargo fmt --check` clean, clippy clean with CI enforcing `-D warnings`.

## Completed in the 2026-07-17 pass

- **Phase 7 — Protocol evolution (v2)**: `protocol_version` gating on
  `Authenticate` (legacy clients get a clear `AuthFailure`); UDP wired per
  SPEC §3 (token handshake binds the client's UDP address, `PlayerInput`
  in / telemetry out over UDP with TCP fallback, per-address rate
  limiting); compact positional telemetry (`TelemetryCompact`, ~60%
  smaller, session-scoped `u8` car indices with a reliable `SessionRoster`
  mapping over TCP); configurable `network.telemetry_divisor` (default 4 →
  60Hz snapshots, replays still record full rate); `gear`/`clutch` inputs
  wired through to physics. Verified by `tests/protocol_test.rs`
  (version rejection, legacy rejection, UDP binding info, full UDP
  handshake/input/telemetry loopback) and a packet-size assertion in
  `network.rs`. The Godot client was NOT updated — it is deprecated.
- **SPEC.md re-scope**: banner replaced; §1/§3 describe the implemented
  v2 protocol; §4.6 rewritten for the 4-wheel 3D model; §5–§8/§12 marked
  implemented and corrected; §9/§10/§11 carry explicit deferred banners.
- **Physics (Phase 4b + deferred)**: per-wheel quasi-static torque
  balance with real wheelspin/lockup behavior, ABS honored, new
  `traction_control_enabled` driver aid, per-wheel angular velocity on
  `CarState`; combined-slip friction ellipse; clutch-slip launch model
  (launches traction-limited, not idle-limited); hybrid `[hybrid]` config
  wired (motor assist + regen + battery SoC); crest fix (grounded cars
  follow the surface down; no airborne bounce over hills); grip retune
  (911 GT3 1.02→1.18, golfcart 0.7→0.75).
- **AI pace**: Monza best lap 3:52 → ~2:45 (field 2:48–3:30);
  trail-brake-aware corner planning, position-refined raceline
  projection, robust off-track recovery (no more orbiting), edge margin
  on line targets; `race_flow_test` lap bound tightened 320s → 230s plus
  an off-track-percentage regression guard.
- **Smaller items**: replay metadata records real player names;
  `TransportLayer::shutdown` no longer holds the write lock across the
  grace sleep; interactive test-runner TUI retired (crossterm dropped);
  clippy burned down to zero and CI ratcheted to `-D warnings`.

## Remaining (deferred by design)

- **AI tactical layer**: overtaking, avoidance, racing side-by-side.
  Cars still share one line and rub; pack racing causes contact-induced
  off-track excursions (guarded at <30% in `race_flow_test`; the fastest
  car runs ~16%). This is the main lever for further pace/realism.
- **Qualification mode** still deliberately behaves as FreePractice
  (see the `GameMode` match in `game_session.rs`).
- **Wheel-spin ODE**: the tire model is quasi-static (torque balance);
  a full per-wheel rotational integration with inertia would enable
  flat-spots, clutch-kick dynamics etc. Not needed at current fidelity.
- **Deferred spec features** (SPEC §9–§11): race templates/rotation/
  scheduling, content hot-reload, SQLite persistence (lap records,
  standings).
- **Godot client**: deprecated; a future client must speak protocol v2
  (see SPEC §3 and `tests/protocol_test.rs` for a reference flow).

## Verification commands

```bash
cd server
cargo test                                   # full suite (18 binaries)
cargo test --test protocol_test              # protocol v2 / UDP loopback
cargo test --test race_flow_test -- --nocapture   # AI race acceptance
cargo test --test determinism_test           # bit-identical sim guard
cargo bench --bench physics_tick             # hot-loop benchmarks
cargo fmt --check && cargo clippy --all-targets -- -D warnings
```
