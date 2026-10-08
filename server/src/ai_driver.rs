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
use crate::racing_line::RacingLineProfile;
use serde::{Deserialize, Serialize};
use uuid::Uuid;

/// Fraction of the car's shift point at which the least skilled AI upshifts;
/// the most skilled shifts right on it.
const AI_NOVICE_SHIFT_FRAC: f32 = 0.9;
/// An AI driver drops a gear once the gear below would land at or under
/// this fraction of its own shift point.
const AI_DOWNSHIFT_HEADROOM: f32 = 0.85;

/// Share of its car's speed profile the least and most skilled drivers run
/// at. The profile itself keeps a margin below the grip limit.
const PROFILE_NOVICE_PACE: f32 = 0.85;
const PROFILE_ACE_PACE: f32 = 0.98;
/// The share of its car's speed profile a driver of `skill_level` runs at.
pub fn profile_pace(skill_level: u8) -> f32 {
    let skill = (skill_level.clamp(MIN_SKILL_LEVEL, MAX_SKILL_LEVEL) - MIN_SKILL_LEVEL) as f32
        / (MAX_SKILL_LEVEL - MIN_SKILL_LEVEL) as f32;
    PROFILE_NOVICE_PACE + (PROFILE_ACE_PACE - PROFILE_NOVICE_PACE) * skill
}

/// Profile points searched either side of the estimated position.
const PROFILE_SEARCH_POINTS: i64 = 24;
/// Time from pressing the brake to the car decelerating at the rate asked.
const BRAKE_BITE_S: f32 = 0.25;

/// Steering: how far ahead (in seconds of travel) the feedforward reads the
/// line's curvature, over what span, how hard the sideways error is pulled
/// in, and how quickly the wheel follows.
const STEER_PREVIEW_S: f32 = 0.15;
const CURVATURE_HALF_SPAN_M: f32 = 6.0;
const STANLEY_GAIN_NOVICE: f32 = 1.2;
const STANLEY_GAIN_ACE: f32 = 2.0;
const STANLEY_SOFTENING_MPS: f32 = 3.0;
const STEER_LAG_NOVICE_S: f32 = 0.08;
fn env_f(name: &str, default: f32) -> f32 {
    std::env::var(name)
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(default)
}
const STEER_LAG_ACE_S: f32 = 0.04;

/// Smooth per-lap noise: control points this far apart along the line, m.
const LAP_NOISE_SPACING_M: f32 = 150.0;
const PACE_NOISE_SALT: u64 = 0x5041_4345;
const WANDER_SALT: u64 = 0x5741_4E44;
/// The wander's control points are this much further apart than the pace
/// noise's.
const WANDER_STRETCH: f32 = 0.5;
/// A driver's wander about its line on a straight, m: the most precise
/// and the least.
const WANDER_PRECISE_M: f32 = 0.05;
const WANDER_IMPRECISE_M: f32 = 0.5;

/// Lateral room an AI driver keeps between its flank and a car alongside.
const TRAFFIC_SIDE_MARGIN_M: f32 = 1.0;
/// Extra longitudinal reach beyond the two half lengths within which a car
/// counts as alongside (a car just ahead diagonally is still a side threat).
const TRAFFIC_ALONGSIDE_M: f32 = 2.0;
/// How far past the two half lengths an AI driver looks for traffic.
const TRAFFIC_WINDOW_M: f32 = 60.0;

/// A car's grip as its driver judges it.
struct GripBudget {
    /// Share of the tyres' grip cornering is using (0..1).
    used: f32,
    /// Share left for driving or braking along the car (the friction circle).
    circle: f32,
    /// 0 while the car points where it goes, 1 once it is plainly sliding.
    slide: f32,
}

/// Another car as an AI driver sees it.
#[derive(Clone, Copy)]
pub struct TrafficCar<'a> {
    pub state: &'a CarState,
    pub length_m: f32,
    pub width_m: f32,
}

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

    /// Follow the raceline at the full `precision`, without the edge margin
    /// every racing AI keeps (`MAX_LINE_PRECISION`). Only the track guide's
    /// lone teaching car sets it: it is there to show the line, and a run
    /// of it that leaves the road is simply driven again.
    #[serde(default)]
    pub exact_line: bool,
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
            exact_line: false,
        }
    }

    /// Create a profile with full customization.
    #[allow(clippy::too_many_arguments)]
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
            exact_line: false,
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
    /// This car's speed profile along the line
    /// ([`crate::racing_line::build`]): corner speeds from its own grip and
    /// downforce, braking points from its own brakes. Without one the driver
    /// falls back to planning on a fixed g budget.
    speed_profile: Option<&'a RacingLineProfile>,
    /// Seconds before each braking zone the driver lifts and coasts to save
    /// fuel ([`Self::with_fuel_save`]); 0 races flat out.
    coast_s: f32,
}

/// The longest a driver saving fuel coasts before a braking zone, s.
pub const MAX_FUEL_SAVE_COAST_S: f32 = 2.0;

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

/// The point of `speeds` the car is at: the profile is the same loop
/// resampled evenly from the same first point, so the line distance is
/// close to right and the nearest point around it settles the rest.
fn profile_point(state: &CarState, speeds: &RacingLineProfile, d_line: f32) -> Option<usize> {
    let n = speeds.points.len();
    if n == 0 || speeds.spacing_m <= 0.0 {
        return None;
    }
    let guess = (d_line / speeds.spacing_m).round() as i64;
    let mut here = guess.rem_euclid(n as i64) as usize;
    let mut best = f32::MAX;
    for offset in -PROFILE_SEARCH_POINTS..=PROFILE_SEARCH_POINTS {
        let i = (guess + offset).rem_euclid(n as i64) as usize;
        let [x, y, _] = speeds.points[i];
        let dist2 = (x - state.pos_x).powi(2) + (y - state.pos_y).powi(2);
        if dist2 < best {
            best = dist2;
            here = i;
        }
    }
    Some(here)
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
            speed_profile: None,
            coast_s: 0.0,
        }
    }

    /// Lift and coast for `coast_s` (up to [`MAX_FUEL_SAVE_COAST_S`])
    /// before every point where the speed profile has the car braking: the
    /// throttle is shut and the brake left alone, so the car rolls into the
    /// braking zone on what it has and burns nothing but idle.
    pub fn with_fuel_save(mut self, coast_s: f32) -> Self {
        self.coast_s = coast_s.clamp(0.0, MAX_FUEL_SAVE_COAST_S);
        self
    }

    /// Plan speeds from the car's own speed profile.
    pub fn with_speed_profile(mut self, speed_profile: Option<&'a RacingLineProfile>) -> Self {
        self.speed_profile = speed_profile;
        self
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
        self.generate_input_in_traffic(state, &[], current_tick, tick_rate_hz)
    }

    /// [`Self::generate_input`] with the rest of the field in view: the
    /// driver keeps a lateral gap to cars alongside and backs off a car ahead
    /// in its lane instead of driving the line through them.
    ///
    /// Without it every car aimed at the same point on the line: off the grid
    /// the side-by-side pairs steered into each other, leaned on each other
    /// nose-in for a second, and came apart pointing half a radian away from
    /// where they were going.
    pub fn generate_input_in_traffic(
        &self,
        state: &CarState,
        traffic: &[TrafficCar<'_>],
        current_tick: u32,
        tick_rate_hz: u16,
    ) -> PlayerInputData {
        if self.track_config.centerline.is_empty() {
            return PlayerInputData::default();
        }
        if state.racecraft.reverse_s > 0.0 {
            return self.back_out(state);
        }
        if state.gear < 0 {
            // Backed out: stop before first will go in (it does not while
            // the car rolls backwards, and in reverse the throttle would
            // carry on backing it up).
            return PlayerInputData {
                brake: 1.0,
                gear: Some(1),
                clutch: Some(1.0),
                ..PlayerInputData::default()
            };
        }

        // Reaction time: pedal and gear decisions are only remade every
        // `interval` ticks and held in between (the physics step copies the
        // applied inputs onto the car state, so "hold" = replay them).
        // Steering is tracked every tick: holding the wheel still for up to
        // 200 ms is 15 m of travel at 75 m/s, enough to miss a corner.
        let interval = ((self.profile.reaction_time_ms as u64 * tick_rate_hz as u64).div_ceil(1000)
            as u32)
            .max(1);
        let decide_pedals = current_tick.is_multiple_of(interval);

        let track_length = self.get_track_length();
        let skill_factor = self.get_skill_factor();

        // Racing line to follow (raceline when available, else centerline)
        let line = self.racing_line();
        let line_length = self.line_length(&line);
        // Map centerline progress onto the (generally slightly different
        // length) racing line. The proportional estimate can be locally off
        // by tens of meters (the raceline is shorter through corners), which
        // shifts every braking point — refine it by projecting the car's
        // position onto the line near the estimate.
        let d_est = if track_length > 0.0 {
            (state.track_progress / track_length * line_length).rem_euclid(line_length.max(1e-3))
        } else {
            0.0
        };
        let d_line = self.refine_line_distance(&line, d_est, state.pos_x, state.pos_y);

        let (target_speed, slowest_ahead_m) = match self.speed_profile {
            Some(speeds) => self.profile_target_speed(state, speeds, d_line),
            None => {
                // Straight-line speed cap scales with skill:
                // 70 skill = ~52 m/s (187 km/h), 110 skill = ~80 m/s (288 km/h)
                let speed_cap = 52.0 + (skill_factor * 28.0);
                (
                    self.plan_target_speed(state, &line, d_line, speed_cap, skill_factor),
                    None,
                )
            }
        };

        // Consistency variation: deterministic noise, amplitude set by the
        // profile's randomness_scale (less consistent drivers wander more).
        // Smooth along the lap and different every lap: a driver takes a
        // corner a little differently from the last time round, rather than
        // flickering every tick. Only ever slower than the plan: held over
        // a whole corner, the fast side the per-tick noise had (damped to
        // 0.3) doubled the time the field spent sliding in the AI survey.
        let consistency_noise = self
            .lap_noise(d_line, state.current_lap, PACE_NOISE_SALT)
            .min(0.0);
        let target_speed = target_speed
            * (1.0
                + consistency_noise
                    * (1.0 - self.profile.consistency)
                    * self.profile.randomness_scale);

        let mut target_speed = target_speed;

        // Speed-scaled look-ahead for steering. Too long a look-ahead cuts
        // straight through chicanes (the car aims past the apex and runs
        // off); too short a look-ahead at launch turns a small lateral
        // offset into near-full-lock steering. 15m floor + speed scaling.
        let look_ahead = (6.0 + state.speed_mps * 0.6).clamp(15.0, 70.0);
        let (mut tx, mut ty) = self.sample_line(&line, d_line + look_ahead);

        // Precision: low-precision drivers track the (safer, wider)
        // centerline more than the optimal raceline. Even perfect drivers
        // are capped below 1.0 — the raceline runs right at the track edge,
        // and tracking it with any control error clips the grass, so a
        // small permanent pull toward the centerline buys an edge margin.
        if matches!(line, RacingLineRef::Raceline { .. }) {
            const MAX_LINE_PRECISION: f32 = 0.85;
            let cap = if self.profile.exact_line {
                1.0
            } else {
                MAX_LINE_PRECISION
            };
            let precision = self.profile.precision.min(cap);
            let d_center = (state.track_progress
                + look_ahead * track_length / line_length.max(1.0))
            .rem_euclid(track_length.max(1e-3));
            let center = self.find_nearest_centerline_point(d_center);
            tx = center.x + (tx - center.x) * precision;
            ty = center.y + (ty - center.y) * precision;
        }

        // Racecraft (`crate::racecraft`): a lane across the road to pass or
        // defend on, eased onto and off, and the driver's own small wander
        // about its line down the straights. Held on the road.
        let racecraft = state.racecraft;
        let shift = self.lane_shift(
            state,
            &line,
            d_line,
            look_ahead,
            (track_length, line_length),
            (tx, ty),
        );
        tx += shift.0;
        ty += shift.1;
        target_speed *= racecraft.pace;
        // A lane inside the line is a tighter corner: v goes with the root
        // of the radius.
        if racecraft.blend > 0.0 {
            if let Some(ahead_m) = slowest_ahead_m {
                let curvature = self.line_curvature(&line, d_line + ahead_m);
                target_speed *= (1.0 - curvature * shift.2).clamp(0.6, 1.0).sqrt();
            }
        }

        // Off-track recovery: aim at the centerline ahead (the middle of
        // the road), not the raceline — chasing an optimal line from the
        // grass leaves the car orbiting a target it can never rejoin. Speed
        // is set from the heading error: when pointing away from the target
        // slow right down and rotate (low-grip grass makes the turn radius
        // at speed larger than the distance to the road — the source of
        // endless orbiting), then drive back at a moderate pace.
        if !state.is_on_track {
            let d_center = (state.track_progress + 20.0).rem_euclid(track_length.max(1e-3));
            let center = self.find_nearest_centerline_point(d_center);
            tx = center.x;
            ty = center.y;

            let heading_err = self
                .normalize_angle((ty - state.pos_y).atan2(tx - state.pos_x) - state.yaw_rad)
                .abs();
            target_speed = if heading_err > 0.9 { 4.0 } else { 12.0 };
        }

        if !traffic.is_empty() {
            let (hx, hy) = self.line_heading(&line, d_line);
            let (ax, ay, speed_limit) = self.avoid_traffic(state, traffic, (hx, hy), (tx, ty));
            tx = ax;
            ty = ay;
            target_speed = target_speed.min(speed_limit);
        }

        let steering = if state.is_on_track {
            // On a racecraft lane the target is a place across the road, so
            // it is held across the road (`hold_lane`), by the blend.
            let hold = if racecraft.blend > 0.0 {
                let d_center = (state.track_progress
                    + look_ahead * track_length / line_length.max(1.0))
                .rem_euclid(track_length.max(1e-3));
                let center = self.find_nearest_centerline_point(d_center);
                let left = (tx - center.x) * -center.heading_rad.sin()
                    + (ty - center.y) * center.heading_rad.cos();
                Some((racecraft.blend, left))
            } else {
                None
            };
            self.track_path(
                state,
                &line,
                d_line,
                look_ahead,
                (tx, ty),
                hold,
                skill_factor,
            )
        } else {
            self.calculate_steering(state, tx, ty, skill_factor)
        };
        let steering = self.limit_steering_to_grip(state, steering);
        let steering = self.smooth_steering(state, steering, skill_factor, tick_rate_hz);

        if !decide_pedals {
            return PlayerInputData {
                throttle: state.throttle_input,
                brake: state.brake_input,
                steering,
                gear: Some(state.gear),
                clutch: Some(state.clutch_input),
                drs: state.drs_allowed,
                headlights: None,
                flash: false,
                ers_mode: None,
                ers_boost: false,
            };
        }

        let (throttle, brake) = self.calculate_throttle_brake(state, target_speed, skill_factor);
        let throttle = self.limit_throttle_to_grip(state, throttle);
        // Saving fuel: off the throttle into the braking zone, on the brake
        // only where the ordinary plan wants it.
        let throttle = if brake <= 0.0 && state.is_on_track && self.coasting(state, d_line) {
            0.0
        } else {
            throttle
        };
        let brake = self.limit_brake_to_grip(state, brake);
        let gear = self.calculate_gear(state, skill_factor);

        // The flap opens whenever the rules allow it: the sim shuts it
        // again the moment the brake goes on.
        PlayerInputData {
            throttle,
            brake,
            steering,
            gear: Some(gear),
            clutch: Some(1.0),
            drs: state.drs_allowed,
            headlights: None,
            flash: false,
            ers_mode: None,
            ers_boost: false,
        }
    }

    /// Backing out of a wall (`crate::racecraft`): stop, select reverse,
    /// and back up with the wheel turned so the nose swings toward the road
    /// ahead (backing, a car turns the other way from its wheels).
    fn back_out(&self, state: &CarState) -> PlayerInputData {
        let total = self.get_track_length();
        let error = crate::racecraft::road_heading_error(state, self.track_config, total);
        let in_reverse = state.gear < 0;
        PlayerInputData {
            throttle: if in_reverse { 0.5 } else { 0.0 },
            brake: if in_reverse { 0.0 } else { 1.0 },
            steering: (-2.0 * error).clamp(-1.0, 1.0),
            gear: Some(-1),
            clutch: Some(1.0),
            drs: false,
            headlights: None,
            flash: false,
            ers_mode: None,
            ers_boost: false,
        }
    }

    /// Steering that follows a path: the line, shifted sideways to pass
    /// through the target point (which carries the precision pull towards
    /// the centerline and any dodge around traffic).
    ///
    /// Feedforward turns the wheels for the curvature just ahead; feedback
    /// corrects the heading error and, through `atan(k·e / v)`, the sideways
    /// error (the Stanley controller). Aiming at a point far up the road
    /// instead cut every fast corner: at 75 m/s the point was 50 m ahead,
    /// on the inside of the bend, and the car followed the chord onto the
    /// grass at a third of its grip.
    ///
    /// `hold` is (weight, m left of the centerline): a lane held across the
    /// road rather than as an offset from the line. An offset from the line
    /// read at the target point and held all the way to it sweeps across
    /// the road wherever the line does (every corner entry): a car passing
    /// on the inside drifted into the car it was passing.
    #[allow(clippy::too_many_arguments)]
    fn track_path(
        &self,
        state: &CarState,
        line: &RacingLineRef<'a>,
        d_line: f32,
        look_ahead: f32,
        (tx, ty): (f32, f32),
        hold: Option<(f32, f32)>,
        skill_factor: f32,
    ) -> f32 {
        let speed = state.speed_mps.max(0.0);
        let wheelbase = self.car_config.wheelbase_m;
        let full_lock = self.car_config.max_steering_angle_rad.max(1e-3);

        // How far off the line the target point wants the car to be.
        let (lax, lay) = self.sample_line(line, d_line + look_ahead);
        let (ahx, ahy) = self.line_heading(line, d_line + look_ahead);
        let wanted_offset = (tx - lax) * -ahy + (ty - lay) * ahx;

        // Sideways error measured at the front axle, positive to the left
        // of the path.
        let (c, s) = (state.yaw_rad.cos(), state.yaw_rad.sin());
        let front_x = state.pos_x + c * 0.5 * wheelbase;
        let front_y = state.pos_y + s * 0.5 * wheelbase;
        let d_front = d_line + 0.5 * wheelbase;
        let (px, py) = self.sample_line(line, d_front);
        let (hx, hy) = self.line_heading(line, d_front);
        let offset = (front_x - px) * -hy + (front_y - py) * hx;
        let error = offset - wanted_offset;

        // Curvature a moment ahead, where the wheels will be once they turn.
        let preview = d_front + (speed * STEER_PREVIEW_S).max(2.0);
        let (h0x, h0y) = self.line_heading(line, preview - CURVATURE_HALF_SPAN_M);
        let (h1x, h1y) = self.line_heading(line, preview + CURVATURE_HALF_SPAN_M);
        let turn = (h0x * h1y - h0y * h1x).atan2(h0x * h1x + h0y * h1y);
        let curvature = turn / (2.0 * CURVATURE_HALF_SPAN_M);
        let feedforward = (wheelbase * curvature).atan();

        let mut heading_error = self.normalize_angle(hy.atan2(hx) - state.yaw_rad);
        let (mut error, mut feedforward, mut curvature) = (error, feedforward, curvature);
        if let Some((weight, lane)) = hold {
            // The same three terms against a path parallel to the
            // centerline, `lane` m to its left, blended in by `weight`.
            let here = self.find_nearest_centerline_point(state.track_progress);
            let lane_heading = self.normalize_angle(here.heading_rad - state.yaw_rad);
            let front_left = -state.lateral_offset_m - 0.5 * wheelbase * lane_heading.sin();
            let ahead = state.track_progress + 0.5 * wheelbase + (speed * STEER_PREVIEW_S).max(2.0);
            let h0 = self
                .find_nearest_centerline_point(ahead - CURVATURE_HALF_SPAN_M)
                .heading_rad;
            let h1 = self
                .find_nearest_centerline_point(ahead + CURVATURE_HALF_SPAN_M)
                .heading_rad;
            // A centerline node's heading is its segment's: nodes a few
            // metres apart, so the span is read off their own distances.
            let span = (self
                .find_nearest_centerline_point(ahead + CURVATURE_HALF_SPAN_M)
                .distance_from_start_m
                - self
                    .find_nearest_centerline_point(ahead - CURVATURE_HALF_SPAN_M)
                    .distance_from_start_m)
                .abs()
                .max(1.0);
            // Curvature of a path `lane` m left of a centerline of
            // curvature k: k / (1 - k·lane).
            let k = self.normalize_angle(h1 - h0) / span;
            let lane_curvature = k / (1.0 - k * lane).max(0.3);
            let w = weight.clamp(0.0, 1.0);
            error += w * ((front_left - lane) - error);
            curvature += w * (lane_curvature - curvature);
            feedforward = (wheelbase * curvature).atan();
            heading_error += w * (lane_heading - heading_error);
        }
        let gain = STANLEY_GAIN_NOVICE + (STANLEY_GAIN_ACE - STANLEY_GAIN_NOVICE) * skill_factor;
        let cross_track =
            (-gain * env_f("AI_KE", 1.0) * error / (speed + STANLEY_SOFTENING_MPS)).atan();
        // Damp the yaw rate against the rate the path asks for: without it
        // the heading and sideways feedback, the wheel's lag and the tyres'
        // own lag made a limit cycle at speed, the car weaving down a
        // straight until the rear let go.
        let yaw_damping = -env_f("AI_KR", 0.25) * (state.angular_vel_yaw - speed * curvature);

        ((feedforward + env_f("AI_KPSI", 1.0) * heading_error + cross_track + yaw_damping)
            / full_lock)
            .clamp(-1.0, 1.0)
    }

    /// A driver's hands are not instant: the wheel moves towards the wanted
    /// angle with a skill-dependent time constant.
    fn smooth_steering(
        &self,
        state: &CarState,
        steering: f32,
        skill_factor: f32,
        tick_rate_hz: u16,
    ) -> f32 {
        let tau = STEER_LAG_NOVICE_S + (STEER_LAG_ACE_S - STEER_LAG_NOVICE_S) * skill_factor;
        let dt = 1.0 / tick_rate_hz.max(1) as f32;
        let blend = 1.0 - (-dt / tau).exp();
        state.steering_input + (steering - state.steering_input) * blend
    }

    /// Whether a driver saving fuel ([`Self::with_fuel_save`]) lifts here:
    /// the profile asks for less speed than the car has somewhere within the
    /// coasting window ahead. Inside the braking reach the ordinary target
    /// has the brake on, and that wins.
    fn coasting(&self, state: &CarState, d_line: f32) -> bool {
        let Some(speeds) = self.speed_profile else {
            return false;
        };
        if self.coast_s <= 0.0 || speeds.spacing_m <= 0.0 {
            return false;
        }
        let Some(here) = profile_point(state, speeds, d_line) else {
            return false;
        };
        let n = speeds.points.len();
        let steps = ((state.speed_mps * self.coast_s) / speeds.spacing_m).ceil() as usize;
        (1..=steps.min(n)).any(|k| speeds.speed_mps[(here + k) % n] < state.speed_mps * 0.95)
    }

    /// Target speed from the car's speed profile: the slowest the profile
    /// asks for between here and where the car will be once the driver has
    /// reacted, scaled by how close to the limit this driver dares to go.
    ///
    /// The profile already brakes for each corner with this car's brakes and
    /// takes it at this car's grip, so an F1 is no longer held to a road
    /// car's cornering budget and a road car no longer arrives at a sweeper
    /// at F1 pace and brakes in the middle of it.
    ///
    /// Also where the slowest point it planned for is, m ahead.
    fn profile_target_speed(
        &self,
        state: &CarState,
        speeds: &RacingLineProfile,
        d_line: f32,
    ) -> (f32, Option<f32>) {
        let n = speeds.points.len();
        let Some(here) = profile_point(state, speeds, d_line) else {
            return (state.speed_mps, None);
        };

        // Reaction plus the time the brakes take to bite.
        let lag_s = self.profile.reaction_time_ms as f32 / 1000.0 + BRAKE_BITE_S;
        // Brakes short of their best (cold carbon, faded steel) stop the car
        // later: look further ahead by the distance they lose.
        let pads = state
            .brake_share_of(self.car_config.brake_material, self.car_config.brake_pads)
            .max(0.3);
        let mass = crate::physics::car_mass_kg(self.car_config, state).max(1.0);
        let decel = (self.car_config.max_brake_force_n / mass).max(1.0);
        let long = state.speed_mps * state.speed_mps / (2.0 * decel) * (1.0 / pads - 1.0);
        let reach = (state.speed_mps * lag_s + long).max(speeds.spacing_m);
        let steps = (reach / speeds.spacing_m).ceil() as usize;
        let (slowest_k, slowest) = (0..=steps.min(n))
            .map(|k| (k, speeds.speed_mps[(here + k) % n]))
            .fold((0, f32::INFINITY), |a, b| if b.1 < a.1 { b } else { a });

        let pace = profile_pace(self.profile.skill_level);
        // The profile is planned on tyres in their window. Cold or cooked
        // tyres grip less, and every speed the grip sets goes with its root;
        // the driver takes a little more off than that (the 0.75 power, not
        // the root), because the grip changes under it through a corner as
        // the tread heats. With the root the AI survey found 13-25% more
        // time off the road; with 0.75, less than before tyres had a
        // temperature at all.
        // Likewise the downforce the dirty air of a car ahead or a tailwind
        // takes away.
        let grip = state.tyre_grip_share() * state.aero_load_share;
        (
            slowest * pace * grip.powf(0.75) * env_f("AI_PACE", 1.0),
            Some(slowest_k as f32 * speeds.spacing_m),
        )
    }

    /// The line's curvature at `d`, 1/m, positive turning left.
    fn line_curvature(&self, line: &RacingLineRef<'a>, d: f32) -> f32 {
        let (h0x, h0y) = self.line_heading(line, d - CURVATURE_HALF_SPAN_M);
        let (h1x, h1y) = self.line_heading(line, d + CURVATURE_HALF_SPAN_M);
        let turn = (h0x * h1y - h0y * h1x).atan2(h0x * h1x + h0y * h1y);
        turn / (2.0 * CURVATURE_HALF_SPAN_M)
    }

    /// How far to move the steering target across the road: (x, y) in the
    /// world, and the move in m left of where the line would have it. Onto
    /// the racecraft's lane by its blend, plus the driver's own wander about
    /// its line where the road runs straight. Never off the road, unless
    /// the line itself is.
    fn lane_shift(
        &self,
        state: &CarState,
        line: &RacingLineRef<'a>,
        d_line: f32,
        look_ahead: f32,
        (track_length, line_length): (f32, f32),
        (tx, ty): (f32, f32),
    ) -> (f32, f32, f32) {
        let racecraft = state.racecraft;
        // Wander: up to half a metre for an imprecise driver, a few
        // centimetres for the most precise, and only where the line is
        // nearly straight (a radius over 100 m), so no apex is missed by it.
        let curvature = self.line_curvature(line, d_line + look_ahead);
        let straightness = (1.0 - curvature.abs() * 100.0).clamp(0.0, 1.0);
        let imprecision = 1.0 - self.profile.precision.clamp(0.0, 1.0);
        let amplitude = WANDER_PRECISE_M + (WANDER_IMPRECISE_M - WANDER_PRECISE_M) * imprecision;
        let wander = if self.profile.exact_line {
            0.0
        } else {
            amplitude
                * straightness
                * self.lap_noise(d_line * WANDER_STRETCH, state.current_lap, WANDER_SALT)
        };
        if racecraft.blend <= 0.0 && wander == 0.0 {
            return (0.0, 0.0, 0.0);
        }

        // The road at the target: its centerline point and left normal.
        let d_center = (state.track_progress + look_ahead * track_length / line_length.max(1.0))
            .rem_euclid(track_length.max(1e-3));
        let center = self.find_nearest_centerline_point(d_center);
        let (nx, ny) = (-center.heading_rad.sin(), center.heading_rad.cos());
        let target_left = (tx - center.x) * nx + (ty - center.y) * ny;
        let wanted = target_left + racecraft.blend * (racecraft.lane_m - target_left) + wander;
        let half = 0.5 * self.car_config.width_m + 0.4;
        let lo = (-center.width_right_m + half).min(target_left);
        let hi = (center.width_left_m - half).max(target_left);
        let left = if lo <= hi {
            wanted.clamp(lo, hi)
        } else {
            target_left
        };
        let shift = left - target_left;
        (nx * shift, ny * shift, shift)
    }

    /// Smooth noise in -1..1 along the line for this driver and lap:
    /// control points every [`LAP_NOISE_SPACING_M`], eased between.
    fn lap_noise(&self, d: f32, lap: u16, salt: u64) -> f32 {
        let x = (d / LAP_NOISE_SPACING_M).max(0.0);
        let i = x.floor();
        let f = x - i;
        let ease = f * f * (3.0 - 2.0 * f);
        let (hi, lo) = self.profile.id.as_u64_pair();
        let seed = hi ^ lo.rotate_left(23) ^ ((lap as u64) << 48);
        let at = |k: f32| crate::wind::hash01(seed ^ (k as u64), salt) * 2.0 - 1.0;
        at(i) + (at(i + 1.0) - at(i)) * ease
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
        // Lateral acceleration budget: ~0.7g (skill 70) to ~0.98g (skill
        // 110), inside the ~1g tire grip of the default car. The corner-speed
        // margin below keeps the plan off the exact limit so imperfect
        // steering doesn't turn every apex into an excursion.
        let a_lat_max = (0.65 + 0.25 * skill_factor) * 9.81 * state.tyre_grip_share();
        // Planned braking decel; higher aggressiveness plans with harder
        // braking, i.e. brakes later. Kept below real braking capability so
        // the plan is always achievable.
        let a_brake = (0.62 + 0.28 * skill_factor)
            * 9.81
            * (0.85 + 0.3 * self.profile.aggressiveness.clamp(0.0, 1.0));

        // Finer sampling and a longer window than the braking distance alone:
        // coarse steps miss tight apexes, which is where corner-entry speed
        // errors (and off-track excursions) come from.
        const STEP_M: f32 = 6.0;
        let window = state.speed_mps * state.speed_mps / (2.0 * a_brake) + 40.0;
        let n_samples = ((window / STEP_M).ceil() as usize).clamp(2, 80);

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
                    // 3% planning margin below the lateral budget
                    let v_corner = (a_lat_max / kappa).sqrt() * 0.97;
                    if v_corner < speed_cap {
                        // Trail-brake release: the friction ellipse leaves a
                        // hard-braking tire no lateral grip, so the usable
                        // decel tapers to zero approaching the corner —
                        // braking must be (nearly) done by turn-in.
                        let s = j as f32 * STEP_M;
                        let a_eff = a_brake * (s / (s + 40.0));
                        let allowed = (v_corner * v_corner + 2.0 * a_eff * s).sqrt();
                        target = target.min(allowed);
                    }
                }
            }

            prev = cur;
            cur = next;
        }
        target
    }

    /// Unit direction of the racing line at `d`.
    fn line_heading(&self, line: &RacingLineRef<'a>, d: f32) -> (f32, f32) {
        let (x0, y0) = self.sample_line(line, d - 2.0);
        let (x1, y1) = self.sample_line(line, d + 2.0);
        let (dx, dy) = (x1 - x0, y1 - y0);
        let len = (dx * dx + dy * dy).sqrt();
        if len < 1e-3 {
            (1.0, 0.0)
        } else {
            (dx / len, dy / len)
        }
    }

    /// Bend the steering target around the cars nearby and cap the speed
    /// behind a car ahead. Works in the line's frame at the car: `along` the
    /// line's heading and `side` positive to its left.
    ///
    /// A car overlapping this one along the line pins the target to its own
    /// side of it, a car width plus [`TRAFFIC_SIDE_MARGIN_M`] away; with a car
    /// on each side the target is the middle and the driver lifts. A car
    /// ahead in the lane caps the speed so the gap closes to a
    /// speed-dependent following distance and no further. Once a car has
    /// cleared the overlap the constraint is gone and the target falls back
    /// to the line, so the field funnels into single file behind it.
    fn avoid_traffic(
        &self,
        state: &CarState,
        traffic: &[TrafficCar<'_>],
        (hx, hy): (f32, f32),
        (tx, ty): (f32, f32),
    ) -> (f32, f32, f32) {
        let (nx, ny) = (-hy, hx);
        let own_len = self.car_config.length_m;
        let own_width = self.car_config.width_m;
        // Where the line would have the car go, sideways from where it is.
        let wanted = (tx - state.pos_x) * nx + (ty - state.pos_y) * ny;
        let mut lo = f32::NEG_INFINITY;
        let mut hi = f32::INFINITY;
        let mut speed_limit = f32::INFINITY;
        let own_fwd = state.vel_x * hx + state.vel_y * hy;

        for other in traffic {
            let o = other.state;
            if o.player_id == state.player_id {
                continue;
            }
            let (rx, ry) = (o.pos_x - state.pos_x, o.pos_y - state.pos_y);
            let along = rx * hx + ry * hy;
            let side = rx * nx + ry * ny;
            let reach_along = 0.5 * (own_len + other.length_m);
            let clear_side = 0.5 * (own_width + other.width_m);
            if along.abs() > reach_along + TRAFFIC_WINDOW_M || side.abs() > clear_side + 4.0 {
                continue;
            }

            if along.abs() < reach_along + TRAFFIC_ALONGSIDE_M {
                // Alongside: stay on our side of it. An exact tie goes by
                // player id so two cars never pick the same side.
                let left_of_us = side > 0.0 || (side == 0.0 && o.player_id > state.player_id);
                let gap = clear_side + TRAFFIC_SIDE_MARGIN_M;
                if left_of_us {
                    hi = hi.min(side - gap);
                } else {
                    lo = lo.max(side + gap);
                }
            } else if along > 0.0 && side.abs() < clear_side + TRAFFIC_SIDE_MARGIN_M {
                // Ahead in our lane: close to the following distance, then
                // hold its pace.
                let other_fwd = o.vel_x * hx + o.vel_y * hy;
                let gap = along - reach_along;
                // Closer in the tow of a car the driver is pulling out to
                // pass.
                let attacking = state.racecraft.tactic == crate::racecraft::Tactic::Attack
                    && state.racecraft.rival == Some(o.player_id);
                let wanted_gap = if attacking {
                    1.5 + 0.15 * own_fwd.max(0.0)
                } else {
                    2.0 + 0.3 * own_fwd.max(0.0)
                };
                let limit = other_fwd + 0.8 * (gap - wanted_gap);
                speed_limit = speed_limit.min(limit.max(0.0));
            }
        }

        if lo == f32::NEG_INFINITY && hi == f32::INFINITY {
            return (tx, ty, speed_limit);
        }

        // Keep the dodge on the road (the line itself may use the edges).
        let edge = 0.5 * own_width + 0.5;
        let road_left = state.lateral_offset_m + self.track_width_left(state) - edge;
        let road_right = state.lateral_offset_m - self.track_width_right(state) + edge;

        let side = if lo > hi {
            // Squeezed between two cars: take the middle and drop back.
            speed_limit = speed_limit.min(own_fwd - 2.0).max(0.0);
            0.5 * (lo + hi)
        } else {
            wanted.clamp(lo, hi)
        };
        let side = if road_right <= road_left {
            side.clamp(road_right, road_left)
        } else {
            side
        };
        let shift = side - wanted;
        (tx + nx * shift, ty + ny * shift, speed_limit)
    }

    fn track_width_left(&self, state: &CarState) -> f32 {
        self.nearest_track_point(state)
            .map_or(5.0, |p| p.width_left_m)
    }

    fn track_width_right(&self, state: &CarState) -> f32 {
        self.nearest_track_point(state)
            .map_or(5.0, |p| p.width_right_m)
    }

    fn nearest_track_point(&self, state: &CarState) -> Option<&TrackPoint> {
        state
            .nearest_centerline_idx
            .and_then(|i| self.track_config.centerline.get(i as usize))
            .or_else(|| Some(self.find_nearest_centerline_point(state.track_progress)))
    }

    /// Refine a proportional line-distance estimate by finding the sample
    /// within ±60m of the estimate that is closest to the car's position.
    /// Corrects the local drift between centerline progress and raceline
    /// arc length, which otherwise shifts braking points by the same error.
    fn refine_line_distance(&self, line: &RacingLineRef<'a>, d_est: f32, x: f32, y: f32) -> f32 {
        const SEARCH_HALF_WINDOW: f32 = 150.0;
        const SEARCH_STEP: f32 = 3.0;
        let mut best_d = d_est;
        let mut best_dist2 = f32::MAX;
        let mut d = d_est - SEARCH_HALF_WINDOW;
        while d <= d_est + SEARCH_HALF_WINDOW {
            let (px, py) = self.sample_line(line, d);
            let dist2 = (px - x) * (px - x) + (py - y) * (py - y);
            if dist2 < best_dist2 {
                best_dist2 = dist2;
                best_d = d;
            }
            d += SEARCH_STEP;
        }
        best_d
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

    /// Steering that points the car at the target: used off the road, where
    /// the target is the middle of the road ahead rather than a path.
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

    /// Keep the front wheels inside what the tyres can use: the lock of the
    /// tightest turn the car can hold at this speed plus the tyre's peak slip
    /// angle, and more towards the side the front axle is already sliding so
    /// a slide can still be caught. Past that the fronts only scrub, and the
    /// car ran wide at full lock through every tight direction change.
    fn limit_steering_to_grip(&self, state: &CarState, steering: f32) -> f32 {
        let config = self.car_config;
        let full_lock = config.max_steering_angle_rad.max(1e-3);
        let downforce = state.downforce_front_n + state.downforce_rear_n;
        let front_travel = self.front_axle_travel_rad(state);
        let toward_travel = (steering.signum() * front_travel).max(0.0);
        let mass = crate::physics::car_mass_kg(config, state);
        let lock = crate::physics::grip_limit_lock_rad(config, mass, state.speed_mps, downforce)
            + config.tire_config.optimal_slip_angle_rad
            + toward_travel;
        let cap = (lock / full_lock).min(1.0);
        steering.clamp(-cap, cap)
    }

    /// Direction the front axle is moving, relative to the car's nose (rad,
    /// + = left). Zero at a crawl, where the angle means nothing.
    fn front_axle_travel_rad(&self, state: &CarState) -> f32 {
        if state.speed_mps < 3.0 {
            return 0.0;
        }
        let (c, s) = (state.yaw_rad.cos(), state.yaw_rad.sin());
        let fwd = state.vel_x * c + state.vel_y * s;
        let lat = -state.vel_x * s
            + state.vel_y * c
            + state.angular_vel_yaw * 0.5 * self.car_config.wheelbase_m;
        lat.atan2(fwd.abs().max(1e-3))
    }

    /// How much of the tyres' grip cornering is taking, and whether the car
    /// is already sliding.
    fn grip_budget(&self, state: &CarState) -> GripBudget {
        let config = self.car_config;
        let downforce = state.downforce_front_n + state.downforce_rear_n;
        let mass = crate::physics::car_mass_kg(config, state).max(1.0);
        let load_ratio = 1.0 + downforce.max(0.0) / (mass * 9.81);
        let grip_g = config.envelope_mu(load_ratio) * load_ratio * state.tyre_grip_share();
        let used =
            (state.g_forces.lateral_g.abs() / (grip_g * env_f("AI_GM", 1.0)).max(0.1)).min(1.0);

        let (c, s) = (state.yaw_rad.cos(), state.yaw_rad.sin());
        let fwd = state.vel_x * c + state.vel_y * s;
        let lat = -state.vel_x * s + state.vel_y * c;
        let body_slip = lat.atan2(fwd.abs().max(1e-3)).abs();
        const SLIDE_START_RAD: f32 = 0.05;
        const SLIDE_FULL_RAD: f32 = 0.15;
        GripBudget {
            used,
            // What is left of the circle after cornering, never below a trickle.
            circle: (1.0 - used * used).max(0.0).sqrt().max(0.15),
            slide: ((body_slip - SLIDE_START_RAD) / (SLIDE_FULL_RAD - SLIDE_START_RAD))
                .clamp(0.0, 1.0),
        }
    }

    /// Brake a driver can use without locking the tyres out of the corner:
    /// the pedal shares the friction circle with cornering the same way the
    /// throttle does (full pedal is about the grip limit on every shipped
    /// car). Braking at half pedal while pulling a g sideways locked the
    /// inside rear at its peak slip and the tail came round.
    fn limit_brake_to_grip(&self, state: &CarState, brake: f32) -> f32 {
        if brake <= 0.0 || state.speed_mps < 3.0 {
            return brake;
        }
        let grip = self.grip_budget(state);
        brake.min(grip.circle).min(1.0 - 0.8 * grip.slide)
    }

    /// Throttle a driver can actually put down. Traction control holds the
    /// driven wheels at their peak slip ratio, which spends the whole friction
    /// budget on drive: full throttle out of a slow corner left the rears no
    /// grip to corner with and the car swapped ends. So the throttle shares
    /// the friction circle with the cornering load, and a car whose tail is
    /// already out gets only enough to keep it from snapping back.
    fn limit_throttle_to_grip(&self, state: &CarState, throttle: f32) -> f32 {
        if throttle <= 0.0 || state.speed_mps < 3.0 {
            return throttle;
        }
        let config = self.car_config;
        let grip = self.grip_budget(state);
        let (used, circle) = (grip.used, grip.circle);
        let slide_cap = 1.0 - 0.85 * grip.slide;

        // The pedal is not the traction: in first gear a third of it can
        // already hold the driven wheels at their peak slip. So if they are
        // using more of their slip than the circle leaves them, back off from
        // what is applied now rather than from what was asked.
        let driven = &state.tires;
        let driven_slip = match config.drivetrain {
            Drivetrain::RWD => driven
                .rear_left
                .slip_ratio
                .max(driven.rear_right.slip_ratio),
            Drivetrain::FWD => driven
                .front_left
                .slip_ratio
                .max(driven.front_right.slip_ratio),
            Drivetrain::AWD => driven
                .rear_left
                .slip_ratio
                .max(driven.rear_right.slip_ratio)
                .max(driven.front_left.slip_ratio)
                .max(driven.front_right.slip_ratio),
        };
        let slip_allowed = config.tire_config.optimal_slip_ratio * circle.max(0.3);
        let traction_cap = if used > 0.3 && driven_slip > slip_allowed {
            (state.throttle_input * 0.8).max(0.1)
        } else {
            1.0
        };

        throttle.min(circle).min(slide_cap).min(traction_cap)
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
        } else if speed_diff <= -1.0 {
            // Brake in proportion to the overshoot: the planner already
            // discounted the braking distance, so a big overshoot needs a
            // firm pedal quickly, but a small one mid-corner only a touch.
            let brake = ((-speed_diff - 0.5) * 0.3).clamp(0.05, 0.7 + 0.3 * skill_factor);
            (0.0, brake)
        } else {
            // Hold speed
            (0.35, 0.0)
        }
    }

    /// Gear selection from the car's own shift points.
    ///
    /// Upshifts come from the same torque-curve crossover the automatic box
    /// uses ([`crate::physics::auto_upshift_rpm`]), so an AI F1 car runs its
    /// engine to 14 000+ and a torquey road car shifts where its torque does
    /// — not at one rev figure for every engine. Skill decides how close to
    /// that point the driver gets: a novice short-shifts, an ace doesn't.
    ///
    /// Downshifts are measured against the lower gear's own shift point: drop
    /// a gear once it would land with [`AI_DOWNSHIFT_HEADROOM`] of its range
    /// to spare. That is also the hysteresis — straight after an upshift the
    /// gear below would land exactly at its shift point, well above the
    /// headroom, so the driver never hunts between two gears.
    fn calculate_gear(&self, state: &CarState, skill_factor: f32) -> i8 {
        let current_gear = state.gear;
        let rpm = state.engine_rpm;
        let ratios = &self.car_config.gear_ratios;

        // Gear count from car config (exclude reverse which is negative)
        let max_gear = ratios.iter().filter(|&&g| g > 0.0).count() as i8;

        // Neutral or reverse: the AI never drives backwards, and a player who
        // finished in reverse hands the car to the cool-down driver in it.
        if current_gear <= 0 {
            return 1;
        }

        let early = AI_NOVICE_SHIFT_FRAC + (1.0 - AI_NOVICE_SHIFT_FRAC) * skill_factor;
        let shift_point =
            |gear: i8| crate::physics::auto_upshift_rpm(self.car_config, gear).map(|u| u * early);

        if current_gear < max_gear && shift_point(current_gear).is_some_and(|up| rpm >= up) {
            return current_gear + 1;
        }

        if current_gear > 1 {
            let (now, down) = (
                ratios[current_gear as usize].abs(),
                ratios[current_gear as usize - 1].abs(),
            );
            let rpm_down = rpm * down / now.max(1e-3);
            if shift_point(current_gear - 1).is_some_and(|up| rpm_down < up * AI_DOWNSHIFT_HEADROOM)
            {
                return current_gear - 1;
            }
        }

        current_gear
    }

    /// Normalize an angle to the range -PI to PI. `rem_euclid`, not `%`: the
    /// remainder keeps the sign of a negative angle, so a car heading due
    /// west with its target just across the ±PI seam read a 6.2 rad error
    /// and turned away from the road at full lock.
    fn normalize_angle(&self, angle: f32) -> f32 {
        let pi = std::f32::consts::PI;
        (angle + pi).rem_euclid(2.0 * pi) - pi
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

/// How many skill points separate the slowest and the quickest driver of a
/// field the host set a level for: close enough to race each other, far
/// enough apart that they do not drive as one car.
pub const AI_FIELD_SPREAD: u8 = 4;

/// A skill level inside the bounds the drivers are built for.
pub fn clamp_skill(skill: u8) -> u8 {
    skill.clamp(MIN_SKILL_LEVEL, MAX_SKILL_LEVEL)
}

/// The skill of each of `count` AI drivers, in the order they are generated.
///
/// With a `level` (the host's pick on the create screen) the field is spread
/// evenly over [`AI_FIELD_SPREAD`] points round it, shifted to stay inside
/// the skill bounds, so a level of 110 is 106-110 rather than 108-112. With
/// none it is the mixed field the server always had: every level from
/// novice to ace, the first drivers the slowest.
pub fn field_skills(count: u8, level: Option<u8>) -> Vec<u8> {
    match level {
        Some(level) => {
            let level = clamp_skill(level);
            let low = level
                .saturating_sub(AI_FIELD_SPREAD / 2)
                .clamp(MIN_SKILL_LEVEL, MAX_SKILL_LEVEL - AI_FIELD_SPREAD);
            (0..count)
                .map(|i| {
                    if count == 1 {
                        level
                    } else {
                        let share = i as f32 / (count - 1) as f32;
                        low + (share * AI_FIELD_SPREAD as f32).round() as u8
                    }
                })
                .collect()
        }
        None => {
            let skill_range = MAX_SKILL_LEVEL - MIN_SKILL_LEVEL;
            let skill_step = if count > 1 {
                skill_range / (count - 1)
            } else {
                0
            };
            (0..count)
                .map(|i| MIN_SKILL_LEVEL + i.saturating_mul(skill_step).min(skill_range))
                .collect()
        }
    }
}

/// Generate `count` AI driver profiles at the skills [`field_skills`] gives
/// for `level` (`None`: the mixed field).
pub fn generate_ai_profiles(count: u8, level: Option<u8>) -> Vec<AiDriverProfile> {
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

    field_skills(count, level)
        .into_iter()
        .enumerate()
        .map(|(i, skill)| {
            let name = AI_NAMES.get(i).unwrap_or(&"AI Driver");
            AiDriverProfile::new(*name, skill)
        })
        .collect()
}

/// Generate a set of default AI driver profiles with varying skill levels:
/// the mixed field, novice to ace.
pub fn generate_default_ai_profiles(count: u8) -> Vec<AiDriverProfile> {
    generate_ai_profiles(count, None)
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
    fn a_chosen_level_spreads_the_field_round_it() {
        // Round the level, every driver within the spread, both ends used.
        let skills = field_skills(5, Some(95));
        assert_eq!(skills, vec![93, 94, 95, 96, 97]);
        // Pushed inside the bounds at either end, never past them.
        let top = field_skills(19, Some(MAX_SKILL_LEVEL));
        assert_eq!(*top.iter().max().unwrap(), MAX_SKILL_LEVEL);
        assert_eq!(
            *top.iter().min().unwrap(),
            MAX_SKILL_LEVEL - AI_FIELD_SPREAD
        );
        let bottom = field_skills(19, Some(0));
        assert_eq!(*bottom.iter().min().unwrap(), MIN_SKILL_LEVEL);
        assert_eq!(
            *bottom.iter().max().unwrap(),
            MIN_SKILL_LEVEL + AI_FIELD_SPREAD
        );
        // A lone driver drives at the level itself.
        assert_eq!(field_skills(1, Some(101)), vec![101]);
        assert!(field_skills(0, Some(90)).is_empty());
    }

    #[test]
    fn the_mixed_field_runs_novice_to_ace() {
        let skills = field_skills(5, None);
        assert_eq!(skills, vec![70, 80, 90, 100, 110]);
        assert_eq!(field_skills(1, None), vec![MIN_SKILL_LEVEL]);
        // Many drivers: the step rounds down, never past the top.
        let many = field_skills(19, None);
        assert!(many
            .iter()
            .all(|s| (MIN_SKILL_LEVEL..=MAX_SKILL_LEVEL).contains(s)));
        let profiles = generate_ai_profiles(3, Some(80));
        assert!(profiles.iter().all(|p| (78..=82).contains(&p.skill_level)));
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
    fn heading_error_wraps_across_the_west_seam() {
        let profile = AiDriverProfile::new("Test", 90);
        let track = TrackConfig::default();
        let car = CarConfig::default();
        let controller = AiDriverController::new(&profile, &track, &car);

        // Pointing just north of west, target just south of west: a small
        // turn to the left, not most of a circle to the right.
        let error = controller.normalize_angle(-3.13 - 3.05);
        assert!((error - 0.103).abs() < 1e-3, "error {error}");
        for angle in [-9.0f32, -3.5, -0.1, 0.0, 3.5, 9.0] {
            let n = controller.normalize_angle(angle);
            assert!((-std::f32::consts::PI..=std::f32::consts::PI).contains(&n));
            let turns = (n - angle) / std::f32::consts::TAU;
            assert!((turns - turns.round()).abs() < 1e-4, "{angle} -> {n}");
        }
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
