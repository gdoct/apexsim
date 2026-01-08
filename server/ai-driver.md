# Feature: AI Driver

This document defines the AI driver feature for the server. The AI driver simulates a human driver by producing the same input commands that a human client would send; these inputs are then processed by the physics model exactly as human inputs are processed.

## Goals
- Provide a server-side AI implementation usable as a drop-in replacement for a human player.
- Produce believable driving behavior that follows the optimal racing line and respects vehicle capabilities.
- Expose tuning parameters so behavior can be adjusted for difficulty, realism, and determinism.
- Support demo mode: run a single-AI session and broadcast telemetry to clients.

## Design Overview

- AI drivers are server-side `Player` subtypes (same interface as human players).
- On track load the AI precomputes a plan from the track's racing-line data and the car's performance characteristics.
- Every server tick the AI emits input commands: acceleration, brake, steering, gear, clutch.
- AI inputs are fed into the same physics processing path as human inputs.

## Initialization

- When a session loads a track, each AI driver:
  - receives the track geometry and racing-line data (including optimal speeds/braking points).
  - obtains the car model and performance parameters (max power, braking, weights, gear ratios).
  - computes a baseline trajectory with target speed profile and brake/accel/shift points.
  - sets its starting grid position from the session start event (same placement as human players).

## Per-tick Behavior

Each server tick the AI produces these inputs (values and types):
- `throttle`: float 0.0..1.0
- `brake`: float 0.0..1.0
- `steering`: float -1.0..1.0 (left..right)
- `gear`: integer (1..n)
- `clutch`: float 0.0..1.0 (if car model uses clutch)

Notes:
- Inputs must be clamped and quantized to match the same ranges used by human-client serialization.
- The AI should apply smoothing/latency models configurable per-difficulty to avoid perfect, jerky control.

## Control Layers

AI architecture should be layered to separate responsibilities:

1. Planning layer
	- Generates target waypoints and speed profile from the racing line and car model.
	- Produces braking/acceleration windows and shift schedules.

2. Tactical layer
	- Reacts to dynamic world state (other cars, collisions, off-track events).
	- Decides overtakes, defensive lines, and safety behaviour for incidents.

3. Low-level controller
	- Converts target speed/heading to raw inputs (throttle/brake/steering/gear) using PID or model predictive controllers.
	- Applies actuator limits, introduces configurable delay and noise.

## Parameters and Tuning

Expose these tuning parameters (per-AI or per-difficulty):
- `aggressiveness` (0..1): higher values result in later braking and earlier throttle
- `precision` (0..1): how closely the AI follows the optimal line
- `reaction_time_ms`: added input latency to simulate human reaction
- `steering_smoothness`: smoothing factor for steering commands
- `randomness_scale`: multiplicative noise applied to inputs for variability

Default sensible values should be provided and overridable via session config.

## Determinism and Replay

- For reproducible runs, AI must be able to run in a deterministic mode where RNG is seeded from the session seed.
- Deterministic mode should disable non-deterministic timers and use fixed-step logic so recorded replays match.

## Telemetry and Events

AI should emit the same telemetry as human players (position, velocity, inputs) so clients can replay camera modes and HUD data.

Additionally the server should publish these AI-specific diagnostics for debugging (optional verbosity levels):
- current target waypoint and target speed
- planned braking/shift points for the next N meters
- controller error terms (e.g., steering error, speed error)

## Demo Mode

- Demo mode starts a session with one AI player by default, broadcasting telemetry to connected clients.
- Replace old ad-hoc demo movement logic with the AI implementation so the camera modes and UI receive standard telemetry.

## Implementation Notes

- Implement AI as a `ServerPlayer` subtype so match-making, session lifecycle, and event routing are unchanged.
- AI must have read-only access to `Track` data structures including racing-line metadata.
- Reuse existing physics and input serialization paths to avoid duplication.
- Avoid duplicating state: AI should not bypass the physics engine (no direct position writes except for debug/restore).

## Testing

- Unit tests for low-level controller correctness (steering conversion, throttle/brake blending).
- Integration tests that run a short session in deterministic mode and assert replay matches recorded telemetry.
- A demo-mode smoke test that starts a server session with one AI and verifies telemetry is sent to clients.

## Decisions

- The AI has access to full-track ‘lookahead’ (entire racing line)
- multi-car tactics can be prioritized: conservative (avoid collisions) or aggressive (attempt overtakes) based on a parameter. ultimately we would define aggressiveness in the game session creation.
- tuning parameter defaults should be stored and edited in `server.toml` session section. 
- Reuse the player telemetry channel with an `is_ai` flag?

## Action Items
- Implement `AiPlayer` type and wire into session creation.
- Replace demo-mode ad-hoc movement with `AiPlayer` usage.
- Add configuration options and deterministic-mode support.


# AI Driver Gear Shifting Logic
The AI driver gear shifting logic is responsible for determining when to shift gears based on the vehicle's speed, engine RPM, and other factors. The following outlines the basic logic for gear shifting:
1. **Define Gear Ratios**: Establish the gear ratios for each gear in the vehicle. This will help determine the speed range for each gear.
2. **Set RPM Thresholds**: Define the RPM thresholds for shifting up and down.
   - Shift up when RPM exceeds 6000.
   - Shift down when RPM drops below 2000.
