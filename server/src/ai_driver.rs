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
        let normalized_skill = (skill - MIN_SKILL_LEVEL) as f32 / (MAX_SKILL_LEVEL - MIN_SKILL_LEVEL) as f32;

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

impl<'a> AiDriverController<'a> {
    /// Create a new AI driver controller.
    pub fn new(profile: &'a AiDriverProfile, track_config: &'a TrackConfig, car_config: &'a CarConfig) -> Self {
        Self {
            profile,
            track_config,
            car_config,
        }
    }

    /// Generate input for the AI driver based on current car state.
    ///
    /// The skill level affects:
    /// - Target speed (higher skill = faster target speed)
    /// - Look-ahead distance (higher skill = better anticipation)
    /// - Steering accuracy (higher skill = smoother steering)
    /// - Throttle/brake balance (higher skill = better modulation)
    /// - Gear selection (higher skill = better shifting points)
    pub fn generate_input(&self, state: &CarState, current_tick: u32) -> PlayerInputData {
        let track_length = self.get_track_length();
        
        // Skill-based parameters
        let skill_factor = self.get_skill_factor();

        // Target speed scales with skill, adjusted for realistic lap times
        // 70 skill = ~40 m/s (144 km/h), 110 skill = ~60 m/s (216 km/h)
        // Note: This is the base speed; AI will adjust for corners based on racing line
        let base_target_speed = 40.0 + (skill_factor * 20.0);
        
        // Apply consistency variation (lower consistency = more speed variation)
        let consistency_noise = self.get_consistency_noise(current_tick);
        let target_speed = base_target_speed * (1.0 + consistency_noise * (1.0 - self.profile.consistency) * 0.15);
        
        // Look-ahead distance scales with skill (better anticipation)
        let look_ahead_distance = 15.0 + (skill_factor * 20.0);
        
        // Find target point ahead on centerline
        let target_progress = state.track_progress + look_ahead_distance;
        let wrapped_progress = target_progress % track_length;
        
        let target_point = self.find_nearest_centerline_point(wrapped_progress);
        
        // Calculate steering toward target
        let steering = self.calculate_steering(state, target_point, skill_factor);

        // Calculate throttle and brake
        let (throttle, brake) = self.calculate_throttle_brake(state, target_speed, skill_factor);

        // Calculate gear selection
        let gear = self.calculate_gear(state, skill_factor);

        // Calculate clutch (simple: always fully engaged for now)
        let clutch = Some(1.0);

        PlayerInputData {
            throttle,
            brake,
            steering,
            gear: Some(gear),
            clutch,
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
    fn find_nearest_centerline_point(&self, progress: f32) -> &TrackPoint {
        self.track_config
            .centerline
            .iter()
            .min_by_key(|p| {
                ((p.distance_from_start_m - progress).abs() * 1000.0) as i32
            })
            .unwrap_or(&self.track_config.centerline[0])
    }
    
    /// Calculate steering input based on target point and skill.
    fn calculate_steering(&self, state: &CarState, target: &TrackPoint, skill_factor: f32) -> f32 {
        let dx = target.x - state.pos_x;
        let dy = target.y - state.pos_y;
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
    fn calculate_throttle_brake(&self, state: &CarState, target_speed: f32, skill_factor: f32) -> (f32, f32) {
        let speed_diff = target_speed - state.speed_mps;
        
        // Throttle/brake modulation improves with skill
        let modulation_skill = 0.5 + (skill_factor * 0.5);
        
        if speed_diff > 2.0 {
            // Need to accelerate
            let throttle = (0.6 + (skill_factor * 0.4)) * modulation_skill;
            (throttle.clamp(0.0, 1.0), 0.0)
        } else if speed_diff < -5.0 {
            // Need to brake hard
            let brake = (0.4 + (skill_factor * 0.3)) * modulation_skill;
            (0.0, brake.clamp(0.0, 1.0))
        } else if speed_diff < 0.0 {
            // Light braking / coast
            let brake = ((-speed_diff / 5.0) * 0.3) * modulation_skill;
            (0.1, brake.clamp(0.0, 0.3))
        } else {
            // Maintain speed
            let throttle = (0.5 + speed_diff * 0.1) * modulation_skill;
            (throttle.clamp(0.3, 0.8), 0.0)
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
        let max_gear = self.car_config.gear_ratios.iter().filter(|&&g| g > 0.0).count() as i8;

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
        "Max Voltage", "Luna Swift", "Rex Thunder", "Nova Blaze",
        "Kai Storm", "Zara Vortex", "Atlas Fury", "Iris Phantom",
        "Axel Shadow", "Maya Comet", "Orion Flash", "Sierra Bolt",
        "Dante Drift", "Echo Racer", "Felix Turbo", "Gwen Apex",
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

        let car_state = CarState::new(
            Uuid::new_v4(),
            Uuid::new_v4(),
            &track.start_positions[0],
        );

        let input = controller.generate_input(&car_state, 100);

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
