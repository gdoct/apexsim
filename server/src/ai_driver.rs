//! AI Driver system for computer-controlled cars.
//!
//! This module implements AI drivers as server-side players that produce the same
//! input commands as human players. The AI follows the specification in ai-driver.md.
//!
//! ## Architecture
//! The AI is structured in three layers:
//! 1. Planning layer: Generates target waypoints and speed profiles from racing line
//! 2. Tactical layer: Reacts to dynamic world state (other cars, collisions)
//! 3. Low-level controller: Converts targets to raw inputs (throttle/brake/steering/gear)
//!
//! AI drivers have configurable skill levels ranging from 70 (slow, beginner-like)
//! to 110 (impossibly fast, unbeatable). Each AI driver has their own profile
//! that defines their behavior characteristics.

use crate::data::*;
use serde::{Deserialize, Serialize};
use uuid::Uuid;

/// Skill level bounds for AI drivers
pub const MIN_SKILL_LEVEL: u8 = 70;
pub const MAX_SKILL_LEVEL: u8 = 110;

/// Default skill level (average driver)
pub const DEFAULT_SKILL_LEVEL: u8 = 90;

/// Profile for an AI-controlled driver.
///
/// Each AI driver has their own profile that determines their driving behavior,
/// including skill level, aggression, consistency, and more.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AiDriverProfile {
    /// Unique identifier for this AI driver
    pub id: PlayerId,

    /// Display name for the AI driver
    pub name: String,

    /// Skill level (70-110). Higher = faster and more accurate.
    /// - 70-80: Slow, makes frequent mistakes
    /// - 81-90: Below average, occasional errors
    /// - 91-100: Average to good driver
    /// - 101-105: Expert level
    /// - 106-110: Superhuman, practically unbeatable
    pub skill_level: u8,

    /// Aggressiveness (0.0-1.0): higher values result in later braking and earlier throttle
    pub aggressiveness: f32,

    /// Precision (0.0-1.0): how closely the AI follows the optimal line
    pub precision: f32,

    /// Reaction time in milliseconds: added input latency to simulate human reaction
    pub reaction_time_ms: u16,

    /// Steering smoothness: smoothing factor for steering commands (0.0-1.0)
    pub steering_smoothness: f32,

    /// Randomness scale: multiplicative noise applied to inputs for variability (0.0-1.0)
    pub randomness_scale: f32,

    /// Consistency (0.0-1.0). Higher = less variation in lap times.
    pub consistency: f32,

    /// Preferred car configuration (if None, uses default car)
    pub preferred_car_id: Option<CarConfigId>,
}

impl AiDriverProfile {
    /// Create a new AI driver profile with the given name and skill level.
    ///
    /// # Arguments
    /// * `name` - Display name for the AI driver
    /// * `skill_level` - Skill level (will be clamped to 70-110)
    ///
    /// # Returns
    /// A new AI driver profile with reasonable defaults for other attributes
    pub fn new(name: impl Into<String>, skill_level: u8) -> Self {
        let skill = skill_level.clamp(MIN_SKILL_LEVEL, MAX_SKILL_LEVEL);
        let normalized_skill =
            (skill - MIN_SKILL_LEVEL) as f32 / (MAX_SKILL_LEVEL - MIN_SKILL_LEVEL) as f32;

        Self {
            id: Uuid::new_v4(),
            name: name.into(),
            skill_level: skill,
            // Derive attributes from skill level with sensible defaults
            aggressiveness: (normalized_skill * 0.6 + 0.2).clamp(0.0, 1.0),
            precision: (normalized_skill * 0.7 + 0.3).clamp(0.0, 1.0),
            reaction_time_ms: ((1.0 - normalized_skill) * 150.0 + 50.0) as u16, // 50-200ms
            steering_smoothness: (normalized_skill * 0.6 + 0.4).clamp(0.0, 1.0),
            randomness_scale: ((1.0 - normalized_skill) * 0.15).clamp(0.0, 1.0),
            consistency: (normalized_skill * 0.5 + 0.4).clamp(0.0, 1.0),
            preferred_car_id: None,
        }
    }

    /// Create a profile with full customization.
    pub fn with_attributes(
        name: impl Into<String>,
        skill_level: u8,
        aggressiveness: f32,
        precision: f32,
        reaction_time_ms: u16,
        steering_smoothness: f32,
        randomness_scale: f32,
        consistency: f32,
    ) -> Self {
        Self {
            id: Uuid::new_v4(),
            name: name.into(),
            skill_level: skill_level.clamp(MIN_SKILL_LEVEL, MAX_SKILL_LEVEL),
            aggressiveness: aggressiveness.clamp(0.0, 1.0),
            precision: precision.clamp(0.0, 1.0),
            reaction_time_ms,
            steering_smoothness: steering_smoothness.clamp(0.0, 1.0),
            randomness_scale: randomness_scale.clamp(0.0, 1.0),
            consistency: consistency.clamp(0.0, 1.0),
            preferred_car_id: None,
        }
    }

    /// Set the preferred car for this AI driver.
    pub fn with_car(mut self, car_id: CarConfigId) -> Self {
        self.preferred_car_id = Some(car_id);
        self
    }
}

impl Default for AiDriverProfile {
    fn default() -> Self {
        Self::new("AI Driver", DEFAULT_SKILL_LEVEL)
    }
}

/// AI driver controller that generates inputs based on the driver profile.
///
/// This implements the three-layer architecture:
/// 1. Planning: Uses racing line data to determine target waypoints and speeds
/// 2. Tactical: Reacts to dynamic conditions (not yet fully implemented)
/// 3. Low-level control: Converts targets to throttle/brake/steering/gear inputs
pub struct AiDriverController<'a> {
    profile: &'a AiDriverProfile,
    track_config: &'a TrackConfig,
    car_config: &'a CarConfig,
}

/// Borrowed view of the line the AI follows: the optimal raceline when the
/// track provides one (with precomputed cumulative distances for O(log n)
/// lookup), otherwise the centerline.
enum RacingLineRef<'a> {
    Raceline {
        points: &'a [RacelinePoint],
        distances: &'a [f32],
    },
    Centerline {
        points: &'a [TrackPoint],
    },
}

impl<'a> AiDriverController<'a> {
    /// Create a new AI driver controller.
    pub fn new(
        profile: &'a AiDriverProfile,
        track_config: &'a TrackConfig,
        car_config: &'a CarConfig,
    ) -> Self {
        Self {
            profile,
            track_config,
            car_config,
        }
    }

    /// Generate input for the AI driver based on current car state.
    ///
    /// Planning follows the raceline (falling back to the centerline when no
    /// raceline data is available) with curvature-based corner speeds: the
    /// AI looks ahead over its braking distance, derives the maximum corner
    /// speed from `v = sqrt(a_lat_max / curvature)` and brakes early enough
    /// (aggressiveness shifts the braking point later) to make each corner.
    ///
    /// Profile parameters:
    /// - `skill_level`: lateral-g budget, straight-line cap, control quality
    /// - `aggressiveness`: later braking points
    /// - `precision`: how tightly the target tracks the raceline vs. the
    ///   centerline
    /// - `randomness_scale` + `consistency`: amplitude of the deterministic
    ///   per-tick speed noise
    /// - `reaction_time_ms`: inputs are only recomputed every
    ///   `ceil(reaction_time_ms / tick_ms)` ticks; between recompute ticks
    ///   the previously applied inputs (as recorded on the car state by the
    ///   physics step) are held. Fully deterministic: everything is a pure
    ///   function of (car state, tick, profile).
    pub fn generate_input(
        &self,
        state: &CarState,
        current_tick: u32,
        tick_rate_hz: u16,
    ) -> PlayerInputData {
        if self.track_config.centerline.is_empty() {
            return PlayerInputData::default();
        }

        // Reaction time: hold the previously applied inputs between
        // recompute ticks. The physics step copies applied inputs verbatim
        // onto the car state, so "hold" = replay the state's inputs.
        let interval = ((self.profile.reaction_time_ms as u64 * tick_rate_hz as u64).div_ceil(1000)
            as u32)
            .max(1);
        if current_tick % interval != 0 {
            return PlayerInputData {
                throttle: state.throttle_input,
                brake: state.brake_input,
                steering: state.steering_input,
                gear: Some(state.gear),
                clutch: Some(state.clutch_input),
            };
        }

        let track_length = self.get_track_length();
        let skill_factor = self.get_skill_factor();

        // Racing line to follow (raceline when available, else centerline)
        let line = self.racing_line();
        let line_length = self.line_length(&line);
        // Map centerline progress onto the (generally slightly different
        // length) racing line.
        let d_line = if track_length > 0.0 {
            (state.track_progress / track_length * line_length).rem_euclid(line_length.max(1e-3))
        } else {
            0.0
        };

        // Straight-line speed cap scales with skill:
        // 70 skill = ~40 m/s (144 km/h), 110 skill = ~60 m/s (216 km/h)
        let speed_cap = 40.0 + (skill_factor * 20.0);

        // Curvature-based corner speed planning
        let target_speed = self.plan_target_speed(state, &line, d_line, speed_cap, skill_factor);

        // Consistency variation: deterministic noise, amplitude set by the
        // profile's randomness_scale (less consistent drivers wander more).
        let consistency_noise = self.get_consistency_noise(current_tick);
        let target_speed = target_speed
            * (1.0
                + consistency_noise
                    * (1.0 - self.profile.consistency)
                    * self.profile.randomness_scale);

        // Off-track recovery: slow down to regain the track
        let target_speed = if state.is_on_track {
            target_speed
        } else {
            target_speed.min(20.0)
        };

        // Speed-scaled look-ahead for steering. The 25m floor matters: a
        // short look-ahead at launch turns a small lateral offset into a
        // near-full-lock steering command that drives the car off the road.
        let look_ahead = (8.0 + state.speed_mps * 0.55).clamp(25.0, 60.0);
        let (mut tx, mut ty) = self.sample_line(&line, d_line + look_ahead);

        // Precision: low-precision drivers track the (safer, wider)
        // centerline more than the optimal raceline.
        if matches!(line, RacingLineRef::Raceline { .. }) && self.profile.precision < 1.0 {
            let d_center = (state.track_progress
                + look_ahead * track_length / line_length.max(1.0))
            .rem_euclid(track_length.max(1e-3));
            let center = self.find_nearest_centerline_point(d_center);
            tx = center.x + (tx - center.x) * self.profile.precision;
            ty = center.y + (ty - center.y) * self.profile.precision;
        }

        // Off-track recovery: aim at the centerline ahead (the middle of the
        // road), not the raceline — chasing an optimal line from the grass
        // leaves the car orbiting a target it can never rejoin.
        if !state.is_on_track {
            let d_center = (state.track_progress + look_ahead).rem_euclid(track_length.max(1e-3));
            let center = self.find_nearest_centerline_point(d_center);
            tx = center.x;
            ty = center.y;
        }

        let steering = self.calculate_steering(state, tx, ty, skill_factor);
        let (throttle, brake) = self.calculate_throttle_brake(state, target_speed, skill_factor);
        let gear = self.calculate_gear(state, skill_factor);

        PlayerInputData {
            throttle,
            brake,
            steering,
            gear: Some(gear),
            clutch: Some(1.0),
        }
    }

    /// Plan the current target speed from upcoming curvature: sample the
    /// racing line over the braking-distance window ahead and take the most
    /// restrictive "speed allowed now so the corner speed is reachable".
    fn plan_target_speed(
        &self,
        state: &CarState,
        line: &RacingLineRef<'a>,
        d_line: f32,
        speed_cap: f32,
        skill_factor: f32,
    ) -> f32 {
        // Lateral acceleration budget: ~0.55g (skill 70) to ~0.9g (skill
        // 110), inside the ~1g tire grip of the default car.
        let a_lat_max = (0.55 + 0.35 * skill_factor) * 9.81;
        // Planned braking decel; higher aggressiveness plans with harder
        // braking, i.e. brakes later. Kept below real braking capability so
        // the plan is always achievable.
        let a_brake = (0.55 + 0.25 * skill_factor)
            * 9.81
            * (0.85 + 0.3 * self.profile.aggressiveness.clamp(0.0, 1.0));

        const STEP_M: f32 = 10.0;
        let window = state.speed_mps * state.speed_mps / (2.0 * a_brake) + 30.0;
        let n_samples = ((window / STEP_M).ceil() as usize).clamp(2, 40);

        let mut target = speed_cap;
        let mut prev = self.sample_line(line, d_line);
        let mut cur = self.sample_line(line, d_line + STEP_M);
        for j in 1..=n_samples {
            let next = self.sample_line(line, d_line + (j as f32 + 1.0) * STEP_M);

            // Curvature from the angle between consecutive segments
            let v1 = (cur.0 - prev.0, cur.1 - prev.1);
            let v2 = (next.0 - cur.0, next.1 - cur.1);
            let len1 = (v1.0 * v1.0 + v1.1 * v1.1).sqrt();
            let len2 = (v2.0 * v2.0 + v2.1 * v2.1).sqrt();
            if len1 > 1e-3 && len2 > 1e-3 {
                let cross = v1.0 * v2.1 - v1.1 * v2.0;
                let dot = v1.0 * v2.0 + v1.1 * v2.1;
                let angle = cross.atan2(dot).abs();
                let kappa = angle / (0.5 * (len1 + len2));
                if kappa > 1e-4 {
                    let v_corner = (a_lat_max / kappa).sqrt();
                    if v_corner < speed_cap {
                        let s = j as f32 * STEP_M;
                        let allowed = (v_corner * v_corner + 2.0 * a_brake * s).sqrt();
                        target = target.min(allowed);
                    }
                }
            }

            prev = cur;
            cur = next;
        }
        target
    }

    /// The racing line to follow: the raceline when present with valid
    /// distance data, otherwise the centerline.
    fn racing_line(&self) -> RacingLineRef<'a> {
        let raceline = &self.track_config.raceline;
        let distances = &self.track_config.raceline_distances;
        if raceline.len() >= 2 && distances.len() == raceline.len() {
            RacingLineRef::Raceline {
                points: raceline,
                distances,
            }
        } else {
            RacingLineRef::Centerline {
                points: &self.track_config.centerline,
            }
        }
    }

    /// Total length of the racing line, including the closing segment back
    /// to the first point (tracks are closed loops).
    fn line_length(&self, line: &RacingLineRef<'a>) -> f32 {
        match line {
            RacingLineRef::Raceline { points, distances } => {
                let last = *distances.last().unwrap_or(&0.0);
                let first = &points[0];
                let end = &points[points.len() - 1];
                let dx = first.x - end.x;
                let dy = first.y - end.y;
                last + (dx * dx + dy * dy).sqrt()
            }
            RacingLineRef::Centerline { points } => {
                let last = points
                    .last()
                    .map(|p| p.distance_from_start_m)
                    .unwrap_or(0.0);
                let first = &points[0];
                let end = &points[points.len() - 1];
                let dx = first.x - end.x;
                let dy = first.y - end.y;
                last + (dx * dx + dy * dy).sqrt()
            }
        }
    }

    /// Sample the racing line position at `distance` (wrap-aware).
    /// O(log n): binary search on the monotonically increasing per-point
    /// distances, then linear interpolation (including across the closing
    /// seam back to the first point).
    fn sample_line(&self, line: &RacingLineRef<'a>, distance: f32) -> (f32, f32) {
        let total = self.line_length(line).max(1e-3);
        let d = distance.rem_euclid(total);
        match line {
            RacingLineRef::Raceline { points, distances } => {
                let idx = distances.partition_point(|&pd| pd <= d);
                // points[idx-1] <= d < points[idx] (idx == len means the
                // closing segment back to points[0])
                let i0 = idx.saturating_sub(1);
                let d0 = distances[i0];
                let (p0x, p0y) = (points[i0].x, points[i0].y);
                let (p1x, p1y, d1) = if idx < points.len() {
                    (points[idx].x, points[idx].y, distances[idx])
                } else {
                    (points[0].x, points[0].y, total)
                };
                let seg = (d1 - d0).max(1e-3);
                let t = ((d - d0) / seg).clamp(0.0, 1.0);
                (p0x + (p1x - p0x) * t, p0y + (p1y - p0y) * t)
            }
            RacingLineRef::Centerline { points } => {
                let idx = points.partition_point(|p| p.distance_from_start_m <= d);
                let i0 = idx.saturating_sub(1);
                let d0 = points[i0].distance_from_start_m;
                let (p0x, p0y) = (points[i0].x, points[i0].y);
                let (p1x, p1y, d1) = if idx < points.len() {
                    (
                        points[idx].x,
                        points[idx].y,
                        points[idx].distance_from_start_m,
                    )
                } else {
                    (points[0].x, points[0].y, total)
                };
                let seg = (d1 - d0).max(1e-3);
                let t = ((d - d0) / seg).clamp(0.0, 1.0);
                (p0x + (p1x - p0x) * t, p0y + (p1y - p0y) * t)
            }
        }
    }

    /// Get the skill factor normalized to 0.0-1.0 range.
    fn get_skill_factor(&self) -> f32 {
        (self.profile.skill_level - MIN_SKILL_LEVEL) as f32
            / (MAX_SKILL_LEVEL - MIN_SKILL_LEVEL) as f32
    }

    /// Generate consistency-based noise for the current tick.
    fn get_consistency_noise(&self, tick: u32) -> f32 {
        // Simple pseudo-random noise based on tick and driver ID
        let seed = (tick as u64).wrapping_mul(self.profile.id.as_u128() as u64);
        let noise = ((seed % 1000) as f32 / 500.0) - 1.0; // -1.0 to 1.0
        noise
    }

    /// Find the nearest centerline point to the given progress distance.
    /// `distance_from_start_m` is monotonically increasing along the
    /// centerline, so this is a binary search — O(log n) instead of a full
    /// scan per AI per tick.
    fn find_nearest_centerline_point(&self, progress: f32) -> &TrackPoint {
        let centerline = &self.track_config.centerline;
        let idx = centerline.partition_point(|p| p.distance_from_start_m < progress);
        match (
            idx.checked_sub(1).and_then(|i| centerline.get(i)),
            centerline.get(idx),
        ) {
            (Some(before), Some(after)) => {
                if (progress - before.distance_from_start_m).abs()
                    <= (after.distance_from_start_m - progress).abs()
                {
                    before
                } else {
                    after
                }
            }
            (Some(before), None) => before,
            (None, Some(after)) => after,
            (None, None) => &centerline[0],
        }
    }

    /// Calculate steering input based on target point and skill.
    fn calculate_steering(
        &self,
        state: &CarState,
        target_x: f32,
        target_y: f32,
        skill_factor: f32,
    ) -> f32 {
        let dx = target_x - state.pos_x;
        let dy = target_y - state.pos_y;
        let target_angle = dy.atan2(dx);
        let angle_diff = target_angle - state.yaw_rad;

        // Normalize angle difference to -PI to PI
        let angle_diff = self.normalize_angle(angle_diff);

        // Steering gain increases with skill (more responsive at higher skill)
        let steering_gain = 1.5 + (skill_factor * 1.5);

        // Apply skill-based smoothing (higher skill = smoother corrections)
        let smoothing = 0.5 + (skill_factor * 0.5);
        let raw_steering = angle_diff * steering_gain;

        (raw_steering * smoothing).clamp(-1.0, 1.0)
    }

    /// Calculate throttle and brake inputs based on current speed and target.
    /// Braking is proportional to the overshoot above the (already
    /// braking-distance-planned) target speed.
    fn calculate_throttle_brake(
        &self,
        state: &CarState,
        target_speed: f32,
        skill_factor: f32,
    ) -> (f32, f32) {
        let speed_diff = target_speed - state.speed_mps;

        if speed_diff >= 1.0 {
            // Accelerate: proportional, capped higher with skill
            let throttle = (0.35 + speed_diff * 0.12).clamp(0.3, 0.75 + 0.25 * skill_factor);
            (throttle, 0.0)
        } else if speed_diff <= -1.5 {
            // Brake proportionally to the overshoot
            let brake = ((-speed_diff - 1.5) * 0.15).clamp(0.1, 0.65 + 0.35 * skill_factor);
            (0.0, brake)
        } else {
            // Hold speed
            (0.35, 0.0)
        }
    }

    /// Calculate gear selection based on engine RPM and skill level.
    ///
    /// Implements the gear shifting logic as per spec:
    /// - Shift up when RPM exceeds upshift threshold (skill-dependent)
    /// - Shift down when RPM drops below downshift threshold
    /// - Higher skill = better timing (closer to optimal RPM range)
    fn calculate_gear(&self, state: &CarState, skill_factor: f32) -> i8 {
        let current_gear = state.gear;
        let rpm = state.engine_rpm;

        // Gear count from car config (exclude reverse which is negative)
        let max_gear = self
            .car_config
            .gear_ratios
            .iter()
            .filter(|&&g| g > 0.0)
            .count() as i8;

        // Skill-based shift points
        // Lower skill = shifts early (conservative), higher skill = shifts near redline
        let upshift_base = 6000.0;
        let upshift_rpm = upshift_base + (skill_factor * 1500.0); // 6000-7500 RPM

        let downshift_base = 2500.0;
        let downshift_rpm = downshift_base - (skill_factor * 500.0); // 2000-2500 RPM

        // Shift up if RPM is too high and not in highest gear
        if rpm > upshift_rpm && current_gear < max_gear && current_gear > 0 {
            return current_gear + 1;
        }

        // Shift down if RPM is too low and not in first gear
        if rpm < downshift_rpm && current_gear > 1 {
            return current_gear - 1;
        }

        // Start in first gear if in neutral
        if current_gear == 0 {
            return 1;
        }

        // Otherwise, maintain current gear
        current_gear
    }

    /// Normalize an angle to the range -PI to PI.
    fn normalize_angle(&self, angle: f32) -> f32 {
        let pi = std::f32::consts::PI;
        ((angle + pi) % (2.0 * pi)) - pi
    }

    /// Get the total track length.
    fn get_track_length(&self) -> f32 {
        self.track_config
            .centerline
            .last()
            .map(|p| p.distance_from_start_m)
            .unwrap_or(1000.0)
    }
}

/// Generate a set of default AI driver profiles with varying skill levels.
///
/// # Arguments
/// * `count` - Number of AI drivers to generate
///
/// # Returns
/// A vector of AI driver profiles with names and varying skill levels
pub fn generate_default_ai_profiles(count: u8) -> Vec<AiDriverProfile> {
    // List of AI driver names
    const AI_NAMES: &[&str] = &[
        "Max Voltage",
        "Luna Swift",
        "Rex Thunder",
        "Nova Blaze",
        "Kai Storm",
        "Zara Vortex",
        "Atlas Fury",
        "Iris Phantom",
        "Axel Shadow",
        "Maya Comet",
        "Orion Flash",
        "Sierra Bolt",
        "Dante Drift",
        "Echo Racer",
        "Felix Turbo",
        "Gwen Apex",
    ];

    let mut profiles = Vec::with_capacity(count as usize);

    for i in 0..count {
        let name = AI_NAMES.get(i as usize).unwrap_or(&"AI Driver");

        // Distribute skill levels across the range
        // First few AIs are easier, last few are harder
        let skill_range = MAX_SKILL_LEVEL - MIN_SKILL_LEVEL;
        let skill_step = if count > 1 {
            skill_range / (count - 1)
        } else {
            0
        };
        let skill_level = MIN_SKILL_LEVEL + (i * skill_step).min(skill_range);

        profiles.push(AiDriverProfile::new(*name, skill_level));
    }

    profiles
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_profile_creation() {
        let profile = AiDriverProfile::new("Test Driver", 85);
        assert_eq!(profile.name, "Test Driver");
        assert_eq!(profile.skill_level, 85);
    }

    #[test]
    fn test_skill_clamping() {
        let low = AiDriverProfile::new("Low", 50);
        assert_eq!(low.skill_level, MIN_SKILL_LEVEL);

        let high = AiDriverProfile::new("High", 150);
        assert_eq!(high.skill_level, MAX_SKILL_LEVEL);
    }

    #[test]
    fn test_default_profiles_generation() {
        let profiles = generate_default_ai_profiles(4);
        assert_eq!(profiles.len(), 4);

        // First should be easier, last should be harder
        assert!(profiles[0].skill_level <= profiles[3].skill_level);
    }

    #[test]
    fn test_ai_input_generation() {
        let profile = AiDriverProfile::new("Test", 90);
        let track = TrackConfig::default();
        let car = CarConfig::default();
        let controller = AiDriverController::new(&profile, &track, &car);

        let car_state = CarState::new(Uuid::new_v4(), Uuid::new_v4(), &track.start_positions[0]);

        let input = controller.generate_input(&car_state, 100, 240);

        assert!(input.throttle >= 0.0 && input.throttle <= 1.0);
        assert!(input.brake >= 0.0 && input.brake <= 1.0);
        assert!(input.steering >= -1.0 && input.steering <= 1.0);
    }

    #[test]
    fn test_skill_affects_target_speed() {
        let slow_profile = AiDriverProfile::new("Slow", MIN_SKILL_LEVEL);
        let fast_profile = AiDriverProfile::new("Fast", MAX_SKILL_LEVEL);
        let track = TrackConfig::default();
        let car = CarConfig::default();

        let slow_controller = AiDriverController::new(&slow_profile, &track, &car);
        let fast_controller = AiDriverController::new(&fast_profile, &track, &car);

        // Skill factor should differ
        assert!(slow_controller.get_skill_factor() < fast_controller.get_skill_factor());
    }
}
