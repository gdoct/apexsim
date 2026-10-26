//! The racing line a driver is shown in the client: the track's raceline
//! (its centerline when it has none) with a speed profile for one car along
//! it, reduced to where the car is flat out, where it brakes and where it is
//! held at the grip limit.
//!
//! The profile is the classic quasi-steady-state lap. Every point gets the
//! fastest speed its curvature allows at the car's grip, downforce included;
//! a forward pass then limits that by what the engine can reach from the
//! point before, and a backward pass by what the brakes can shed before the
//! point after. Where the backward pass sets the speed the car is braking,
//! where the forward pass does it is accelerating, and where neither does it
//! is cornering at the limit.
//!
//! Nothing here feeds physics. The same profile is the AI's speed plan, the
//! driving aid sent to a joining player as `ServerMessage::RacingLine`, and
//! the basis of the session's fuel estimate (`lap_fuel_liters`). It is
//! planned for the car as it stands with a given fuel load
//! (`build_laden`), since a full tank corners, brakes and pulls slower
//! than an empty one. The player's aid (`build`) counts a hybrid's motor
//! above its minimum speed, as Balanced mode paces the lap budget
//! (`deploy_kj_per_lap`); the AI's plan counts it at full power.

use crate::data::{CarConfig, Drivetrain, HybridAxle, TrackConfig};
use crate::physics::AIR_DENSITY;

const GRAVITY: f32 = 9.81;

/// Distance between the points of a built line, in metres.
pub const SAMPLE_SPACING_M: f32 = 2.5;

/// Half the chord curvature is measured over. Short enough to see a hairpin
/// at its real radius, long enough that the 5 m kinks of a digitised
/// raceline do not read as corners.
const CURVATURE_HALF_CHORD_M: f32 = 7.5;

/// Planning margins below the car's grip: the line is advice for a human, so
/// its corner speeds and braking points leave room for imperfect inputs.
const LATERAL_GRIP_MARGIN: f32 = 0.93;
const BRAKING_GRIP_MARGIN: f32 = 0.88;

/// Speeds closer than this count as the same when deciding which pass set a
/// point's speed.
const SPEED_EPSILON_MPS: f32 = 0.25;

/// A braking zone that sheds less than this is a lift, not a stop: shown as
/// partial throttle rather than a brake marker.
const LIFT_THRESHOLD_MPS: f32 = 3.0;

/// Phase runs shorter than this are absorbed by the run before them, so a
/// single stray point does not flicker a different colour.
const MIN_RUN_M: f32 = 6.0;

/// What the driver does at a point of the line.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LinePhase {
    /// Accelerating as hard as the engine and tyres allow, or flat out at
    /// top speed.
    Throttle,
    /// At the grip limit through a corner, or lifting for a kink: the
    /// throttle is somewhere in between.
    Partial,
    /// Braking for the corner ahead.
    Brake,
}

impl LinePhase {
    /// Wire value: 0 throttle, 1 partial, 2 brake.
    pub fn as_u8(self) -> u8 {
        match self {
            LinePhase::Throttle => 0,
            LinePhase::Partial => 1,
            LinePhase::Brake => 2,
        }
    }
}

/// A racing line with its speed profile, as a closed loop of evenly spaced
/// points in the server frame (metres). The segment from the last point back
/// to the first closes the lap.
#[derive(Debug, Clone)]
pub struct RacingLineProfile {
    pub spacing_m: f32,
    pub points: Vec<[f32; 3]>,
    pub speed_mps: Vec<f32>,
    pub phases: Vec<LinePhase>,
    /// The speeds planned with a hybrid's motor at full power wherever it
    /// may drive, the lap budget not paced: what the AI caps itself at its
    /// skill's share of (`ai_driver`), as optimistic about the motor as
    /// the plan is about the engine (its peak at every speed). The same
    /// as `speed_mps` but for a car with a lap budget.
    pub unpaced_mps: Vec<f32>,
}

/// Speed at which the envelope takes a load-sensitive tyre's coefficient:
/// a typical corner, where the line's speed is decided.
const ENVELOPE_REFERENCE_SPEED_MPS: f32 = 40.0;

/// Speed step of the envelope's table of the aero posture factor, m/s.
const AERO_TABLE_STEP_MPS: f32 = 2.0;
/// The table reaches this speed and holds the last value past it.
const AERO_TABLE_TOP_MPS: f32 = 120.0;

/// The car as a point mass: what limits its speed along a line.
///
/// Every limit takes `road`, the road's grip at the point against the grip
/// the envelope was made for: 1 for a whole lap planned on one grip, the
/// road state's figure along the aid's window ([`LineWindow`]).
#[derive(Debug, Clone)]
struct CarEnvelope {
    /// Tyre grip times track grip.
    mu: f32,
    /// A load-sensitive tyre's grip times the track's at the load the car
    /// carries at speed (`CarConfig::envelope_mu`: the downforce, and the
    /// lateral transfer at the limit), every [`AERO_TABLE_STEP_MPS`] from
    /// standstill; empty for a linear tyre, whose grip is `mu` at any load.
    mu_table: Vec<f32>,
    /// Downforce per unit mass per (m/s)², so downforce accel = k · v² ·
    /// the posture's factor at v.
    downforce_k: f32,
    /// What the car's ride height makes of its downforce at steady speed
    /// (`aero::steady_downforce_factor`), every [`AERO_TABLE_STEP_MPS`]
    /// from standstill; empty for a car whose downforce does not change.
    aero_factor: Vec<f32>,
    /// Drag per unit mass per (m/s)².
    drag_k: f32,
    rolling_accel: f32,
    /// Peak engine power at the wheels, W, and the car's mass, kg.
    engine_w: f32,
    mass: f32,
    /// A hybrid's motor: what it gives at a speed, by its minimum speed,
    /// its torque curve at the revs it turns at and its power.
    motor: Option<crate::hybrid::MotorPlan>,
    /// Share of the car's weight on the driven wheels.
    driven_fraction: f32,
    /// A front-axle motor on a car whose engine drives only the rears: the
    /// share of the weight on the fronts, the traction the motor has of
    /// its own. `None`: the motor shares the engine's driven wheels.
    motor_front_fraction: Option<f32>,
    /// Most the brakes can do on their own, m/s².
    brake_accel: f32,
    /// Speed at the limiter in top gear.
    top_speed: f32,
}

impl CarEnvelope {
    /// The car at `mass` kg: its dry mass and whatever fuel it carries, in
    /// air `air_density_ratio` times as dense as the reference day's: the
    /// aero and a combustion engine are filed at that day.
    fn new(car: &CarConfig, track_grip: f32, mass: f32, air_density_ratio: f32) -> Self {
        let mass = mass.max(1.0);
        let air = air_density_ratio;
        let breath = crate::physics::engine_density_factor(car, air);
        let lift = car.lift_coefficient_front + car.lift_coefficient_rear;
        let driven_fraction = match car.drivetrain {
            Drivetrain::AWD => 1.0,
            Drivetrain::RWD => 1.0 - car.weight_distribution_front,
            Drivetrain::FWD => car.weight_distribution_front,
        };
        let motor_front_fraction = (car.hybrid.enabled
            && car.hybrid.axle == HybridAxle::Front
            && car.drivetrain == Drivetrain::RWD)
            .then(|| car.weight_distribution_front.clamp(0.2, 1.0));
        let downforce_k = 0.5 * AIR_DENSITY * air * car.frontal_area_m2 * (-lift).max(0.0) / mass;
        let aero_factor: Vec<f32> = if car.aero.is_constant() && car.aero.speed_map.is_empty() {
            Vec::new()
        } else {
            (0..=(AERO_TABLE_TOP_MPS / AERO_TABLE_STEP_MPS) as usize)
                .map(|i| crate::aero::steady_downforce_factor(car, i as f32 * AERO_TABLE_STEP_MPS))
                .collect()
        };
        let load_sensitive = car.tire_config.load_sensitivity_front != 1.0
            || car.tire_config.load_sensitivity_rear != 1.0;
        let mu_table = if load_sensitive {
            (0..=(AERO_TABLE_TOP_MPS / AERO_TABLE_STEP_MPS) as usize)
                .map(|i| {
                    let v = i as f32 * AERO_TABLE_STEP_MPS;
                    let factor = aero_factor.get(i).copied().unwrap_or(1.0);
                    car.envelope_mu(1.0 + downforce_k * factor * v * v / GRAVITY) * track_grip
                })
                .collect()
        } else {
            Vec::new()
        };
        Self {
            // A load-sensitive tyre grips less per newton at speed, where the
            // downforce loads it: plan with its coefficient at the load it
            // carries at the reference speed. A linear tyre's is
            // `grip_coefficient`, to the bit.
            mu: car.envelope_mu(1.0 + downforce_k * ENVELOPE_REFERENCE_SPEED_MPS.powi(2) / GRAVITY)
                * track_grip,
            mu_table,
            downforce_k,
            aero_factor,
            drag_k: 0.5 * AIR_DENSITY * air * car.drag_coefficient * car.frontal_area_m2 / mass,
            rolling_accel: car.tire_config.rolling_resistance * GRAVITY,
            // Peak power over the whole run: a real gearbox spends part of
            // each gear below the peak, so this reaches a little more speed
            // than the car does, which errs towards braking early.
            engine_w: car.max_engine_power_w * breath * car.transmission.efficiency,
            mass,
            motor: crate::hybrid::MotorPlan::new(car),
            driven_fraction: driven_fraction.clamp(0.2, 1.0),
            motor_front_fraction,
            brake_accel: car.max_brake_force_n / mass,
            top_speed: top_gear_speed(car),
        }
    }

    /// Downforce per unit mass per (m/s)² at `speed`: the filed figure times
    /// what the car's ride height makes of it there.
    fn downforce_k_at(&self, speed: f32) -> f32 {
        if self.aero_factor.is_empty() {
            return self.downforce_k;
        }
        let x = (speed.max(0.0) / AERO_TABLE_STEP_MPS).min((self.aero_factor.len() - 1) as f32);
        let i = (x as usize).min(self.aero_factor.len() - 2);
        let t = x - i as f32;
        self.downforce_k * (self.aero_factor[i] * (1.0 - t) + self.aero_factor[i + 1] * t)
    }

    /// The tyres' coefficient (times the track's grip) at `speed`.
    fn mu_at(&self, speed: f32) -> f32 {
        if self.mu_table.is_empty() {
            return self.mu;
        }
        let x = (speed.max(0.0) / AERO_TABLE_STEP_MPS).min((self.mu_table.len() - 1) as f32);
        let i = (x as usize).min(self.mu_table.len() - 2);
        let t = x - i as f32;
        self.mu_table[i] * (1.0 - t) + self.mu_table[i + 1] * t
    }

    /// Total grip the tyres offer at `speed`, m/s².
    fn grip_accel(&self, speed: f32, road: f32) -> f32 {
        self.mu_at(speed) * road * (GRAVITY + self.downforce_k_at(speed) * speed * speed)
    }

    /// Fastest speed a corner of curvature `kappa` can be taken at, capped at
    /// top speed. Solves κ·v² = μ·m·(g + k·v²) for v; with a ride-height
    /// car k depends on v, and with a load-sensitive tyre μ does, so it is
    /// solved again at the answer a few times (both move slowly with speed
    /// and this settles at once).
    fn corner_speed(&self, kappa: f32, road: f32) -> f32 {
        let solve = |k: f32, mu: f32| {
            let mu = mu * LATERAL_GRIP_MARGIN;
            let denominator = kappa - mu * k;
            if denominator <= 1e-6 {
                self.top_speed
            } else {
                (mu * GRAVITY / denominator).sqrt().min(self.top_speed)
            }
        };
        let mut v = solve(self.downforce_k, self.mu * road);
        if !self.aero_factor.is_empty() || !self.mu_table.is_empty() {
            for _ in 0..4 {
                v = solve(self.downforce_k_at(v), self.mu_at(v) * road);
            }
        }
        v
    }

    /// [`Self::grip_accel`] with `tail` m/s of wind behind the car along
    /// its way (negative into it): the downforce is the airflow's, the
    /// tyres' coefficient the car's speed's.
    fn grip_accel_in(&self, speed: f32, road: f32, tail: f32) -> f32 {
        if tail == 0.0 {
            return self.grip_accel(speed, road);
        }
        let air = (speed - tail).max(0.0);
        self.mu_at(speed) * road * (GRAVITY + self.downforce_k_at(air) * air * air)
    }

    /// [`Self::corner_speed`] with `tail` m/s of wind behind the car along
    /// the corner: κ·v² = μ·(g + k·(v − tail)²) solved for v, k and μ
    /// again at the answer.
    fn corner_speed_in(&self, kappa: f32, road: f32, tail: f32) -> f32 {
        if tail == 0.0 {
            return self.corner_speed(kappa, road);
        }
        let solve = |k: f32, mu: f32| {
            let mu = mu * LATERAL_GRIP_MARGIN;
            let a = kappa - mu * k;
            if a <= 1e-6 {
                return self.top_speed;
            }
            let b = 2.0 * mu * k * tail;
            let c = -mu * (GRAVITY + k * tail * tail);
            let disc = b * b - 4.0 * a * c;
            ((-b + disc.max(0.0).sqrt()) / (2.0 * a)).clamp(0.0, self.top_speed)
        };
        let mut v = self.corner_speed(kappa, road);
        for _ in 0..4 {
            v = solve(
                self.downforce_k_at((v - tail).max(0.0)),
                self.mu_at(v) * road,
            );
        }
        v
    }

    /// Grip left along the car once `kappa` at `speed` has taken its share
    /// sideways (the friction circle), with `tail` m/s of wind behind it.
    fn longitudinal_grip_in(&self, speed: f32, kappa: f32, road: f32, tail: f32) -> f32 {
        let total = self.grip_accel_in(speed, road, tail);
        let lateral = kappa * speed * speed;
        let used = (lateral / total.max(1e-3)).min(1.0);
        total * (1.0 - used * used).sqrt()
    }

    /// The motor's power the plan counts on at `speed`, W, deploying
    /// `share` of it.
    fn hybrid_w_at(&self, speed: f32, share: f32) -> f32 {
        self.motor.as_ref().map_or(0.0, |m| m.power_w(speed, share))
    }

    /// Net forward acceleration at full throttle with the motor deploying
    /// `hybrid_share` of its power; `grade` is sin of the slope, positive
    /// uphill.
    /// A front-axle motor on an RWD car puts its power down through the
    /// fronts' own grip, beside the engine's through the rears'.
    fn acceleration(
        &self,
        speed: f32,
        kappa: f32,
        grade: f32,
        hybrid_share: f32,
        road: f32,
    ) -> f32 {
        self.acceleration_in(speed, kappa, grade, hybrid_share, road, 0.0)
    }

    /// [`Self::acceleration`] with `tail` m/s of wind behind the car in its
    /// downforce (the drag is still the still air's: the caller adds what
    /// the wind takes off it).
    fn acceleration_in(
        &self,
        speed: f32,
        kappa: f32,
        grade: f32,
        hybrid_share: f32,
        road: f32,
        tail: f32,
    ) -> f32 {
        let grip = self.longitudinal_grip_in(speed, kappa, road, tail);
        let traction = grip * self.driven_fraction;
        let drive = match self.motor_front_fraction {
            None => {
                let power = (self.engine_w + self.hybrid_w_at(speed, hybrid_share))
                    / self.mass
                    / speed.max(5.0);
                power.min(traction)
            }
            Some(front) => {
                let per_w = 1.0 / self.mass / speed.max(5.0);
                (self.engine_w * per_w).min(traction)
                    + (self.hybrid_w_at(speed, hybrid_share) * per_w).min(grip * front)
            }
        };
        drive - self.drag_k * speed * speed - self.rolling_accel - GRAVITY * grade
    }

    /// Deceleration at the braking limit (positive); `grade` as above.
    fn deceleration(&self, speed: f32, kappa: f32, grade: f32, road: f32) -> f32 {
        self.deceleration_in(speed, kappa, grade, road, 0.0)
    }

    /// [`Self::deceleration`] with `tail` m/s of wind behind the car in its
    /// downforce, the drag as for [`Self::acceleration_in`].
    fn deceleration_in(&self, speed: f32, kappa: f32, grade: f32, road: f32, tail: f32) -> f32 {
        let tyres = self.longitudinal_grip_in(speed, kappa, road, tail) * BRAKING_GRIP_MARGIN;
        tyres.min(self.brake_accel)
            + self.drag_k * speed * speed
            + self.rolling_accel
            + GRAVITY * grade
    }
}

/// Speed at the rev limiter in top gear, or a generous cap for a car with no
/// usable gearing data.
fn top_gear_speed(car: &CarConfig) -> f32 {
    let top_ratio = car
        .gear_ratios
        .iter()
        .copied()
        .filter(|&ratio| ratio > 0.0)
        .fold(f32::INFINITY, f32::min);
    let rpm = if car.engine.rev_limiter_rpm > 0.0 {
        car.engine.rev_limiter_rpm
    } else {
        car.max_engine_rpm
    };
    let overall = top_ratio * car.final_drive_ratio;
    if !overall.is_finite() || overall <= 0.0 || rpm <= 0.0 {
        return 120.0;
    }
    rpm / 60.0 * std::f32::consts::TAU * car.wheel_radius_m / overall
}

/// Build the racing line `car` should follow on `track` with a dry tank,
/// or `None` when the track has too few points to make a loop.
///
/// This is the player's aid: a hybrid's motor counts above its minimum
/// speed, at what Balanced mode's pacing of the lap budget gives it.
pub fn build(track: &TrackConfig, car: &CarConfig) -> Option<RacingLineProfile> {
    plan_line(track, car, 0.0, 1.0)
}

/// [`build`] for the car as a driver has it now: `car` with their setup,
/// carrying `fuel_liters`, on tyres and a road worth `grip_share` of the
/// planned grip (`CarState::tyre_grip_share`: compound, wear, heat, a wet
/// or rubbered line). The aid the server sends again as these move.
pub fn build_driven(
    track: &TrackConfig,
    car: &CarConfig,
    fuel_liters: f32,
    grip_share: f32,
) -> Option<RacingLineProfile> {
    plan_line(track, car, fuel_liters, grip_share)
}

/// [`build`] for the car carrying `fuel_liters`: the extra mass costs
/// braking, acceleration and, on a downforce car, cornering speed.
///
/// This is the AI's speed plan and the fuel estimate's lap, a hybrid's
/// motor counted as the aid counts it: above its minimum speed, paced over
/// the lap budget. The AI caps itself at its skill's share of
/// [`RacingLineProfile::unpaced_mps`], the plan with the motor unpaced: on
/// the straights a cap at a share of a plan as slow as the car holds it
/// below what the car does (on the paced plan an F1 AI lapped Monza 2 s
/// slower).
pub fn build_laden(
    track: &TrackConfig,
    car: &CarConfig,
    fuel_liters: f32,
) -> Option<RacingLineProfile> {
    plan_line(track, car, fuel_liters, 1.0)
}

/// The line for `car` carrying `fuel_liters` on `grip_share` of the
/// track's grip, a hybrid's motor counted as the rules and Balanced mode
/// let it drive.
fn plan_line(
    track: &TrackConfig,
    car: &CarConfig,
    fuel_liters: f32,
    grip_share: f32,
) -> Option<RacingLineProfile> {
    let source: Vec<[f32; 3]> = if track.raceline.len() >= 3 {
        track.raceline.iter().map(|p| [p.x, p.y, p.z]).collect()
    } else {
        track.centerline.iter().map(|p| [p.x, p.y, p.z]).collect()
    };
    let points = resample_loop(&source, SAMPLE_SPACING_M)?;
    let n = points.len();
    let spacing = loop_length(&points) / n as f32;

    let (mass, _) = car.laden(fuel_liters);
    let envelope = CarEnvelope::new(
        car,
        track.track_surface.base_grip * grip_share,
        mass,
        track.track_surface.air_density_ratio,
    );
    let plan = |share: &[f32]| plan_speeds(track, car, &points, spacing, &envelope, share);
    // A lap budget (`[hybrid] deploy_kj_per_lap`): the motor cannot drive
    // at full power everywhere the throttle is open. Balanced mode paces
    // it, so the plan does: planned at full power, the lap is driven
    // through the pacing to see what the motor gives at each point, and
    // planned again on that, a few rounds (less power is more time on the
    // throttle, which the pacing then pays for).
    let mut share = vec![1.0; n];
    let mut profile = plan(&share);
    let unpaced = profile.speed_mps.clone();
    if let (Some(budget), Some(motor)) = (car.hybrid.deploy_kj_per_lap, envelope.motor.as_ref()) {
        for _ in 0..3 {
            let paced = paced_shares(&profile, car, motor, budget);
            if paced == share {
                break;
            }
            share = paced;
            profile = plan(&share);
        }
    }
    profile.unpaced_mps = unpaced;
    Some(profile)
}

/// What Balanced mode (`crate::hybrid`) deploys at each point of a lap of
/// `profile`, as a share of the motor's power, on a lap budget of
/// `budget_kj`: the share of the budget left over the share of the lap
/// left, from the line, spending only where the line is on the throttle
/// above the motor's minimum speed.
fn paced_shares(
    profile: &RacingLineProfile,
    car: &CarConfig,
    motor: &crate::hybrid::MotorPlan,
    budget_kj: f32,
) -> Vec<f32> {
    let n = profile.speed_mps.len();
    let mut spent_kj = 0.0;
    (0..n)
        .map(|i| {
            let lap_share = i as f32 / n as f32;
            let share = crate::hybrid::balanced_pace(
                spent_kj / budget_kj.max(1e-3),
                lap_share,
                car.hybrid.deploy_early,
            );
            let v = profile.speed_mps[i];
            if profile.phases[i] == LinePhase::Throttle {
                spent_kj += motor.power_w(v, share) / 1000.0 * profile.spacing_m / v.max(1.0);
            }
            share
        })
        .collect()
}

/// The speed profile and phases along `points` for the car `envelope`
/// describes, its motor deploying `hybrid_share` of its power at each
/// point.
fn plan_speeds(
    track: &TrackConfig,
    car: &CarConfig,
    points: &[[f32; 3]],
    spacing: f32,
    envelope: &CarEnvelope,
    hybrid_share: &[f32],
) -> RacingLineProfile {
    let n = points.len();
    let kappa = curvature(points, spacing);
    let grade = grades(points, spacing);

    let corner: Vec<f32> = kappa
        .iter()
        .map(|&k| envelope.corner_speed(k, 1.0))
        .collect();
    let drag_scale = drag_scales(track, car, n, spacing);

    // Both passes start from the slowest corner, where the speed is set by
    // the corner alone, so one lap each is enough for a closed loop.
    let slowest = (0..n)
        .min_by(|&a, &b| corner[a].total_cmp(&corner[b]))
        .unwrap_or(0);

    let mut forward = corner.clone();
    for step in 1..n {
        let i = (slowest + step) % n;
        let prev = (i + n - 1) % n;
        let v = forward[prev];
        let a = envelope.acceleration(v, kappa[prev], grade[prev], hybrid_share[prev], 1.0)
            + envelope.drag_k * v * v * (1.0 - drag_scale[prev]);
        let reachable = (v * v + 2.0 * a * spacing).max(0.0).sqrt();
        forward[i] = forward[i].min(reachable);
    }

    let mut backward = corner.clone();
    for step in 1..n {
        let i = (slowest + n - step) % n;
        let next = (i + 1) % n;
        let v = backward[next];
        let d = envelope.deceleration(v, kappa[i], grade[i], 1.0);
        let enterable = (v * v + 2.0 * d * spacing).max(0.0).sqrt();
        backward[i] = backward[i].min(enterable);
    }

    let speed: Vec<f32> = (0..n).map(|i| forward[i].min(backward[i])).collect();
    let mut phases: Vec<LinePhase> = (0..n)
        .map(|i| phase_of(forward[i], backward[i], corner[i], envelope.top_speed))
        .collect();

    downgrade_lifts(&mut phases, &speed, true);
    absorb_short_runs(&mut phases, (MIN_RUN_M / spacing).ceil() as usize, true);

    RacingLineProfile {
        spacing_m: spacing,
        points: points.to_vec(),
        unpaced_mps: speed.clone(),
        speed_mps: speed,
        phases,
    }
}

/// How far past the car the aid's window reaches, m: beyond the 400 m the
/// client draws the dots to, so the stretch in view is always planned.
pub const WINDOW_AHEAD_M: f32 = 800.0;

/// Planned beyond the window and not sent, m: a braking zone at the
/// window's end is planned from the corner it is for. Longer than any
/// braking zone.
const WINDOW_TAIL_M: f32 = 600.0;

/// How often a human's window is planned again, Hz.
pub const WINDOW_HZ: f32 = 5.0;

/// Points either side of the one the car was last found at that the next
/// search looks through: 100 m, five times what a car covers between plans
/// at 360 km/h.
const LOCATE_REACH: usize = 40;

/// A car further than this from the line is not on it (a run-off, the
/// grass): no window is planned for it.
pub const WINDOW_MAX_OFF_LINE_M: f32 = 30.0;

/// The racing line aid's next stretch, planned again a few times a second
/// from the car as it is (`GameSession::racing_line_window`): its speed
/// now, its tyres and fuel, the road state's grip along the stretch and
/// the wind along it. The lap's line ([`build_driven`]) is planned at a
/// lap's end on one grip for the whole lap and assumes the car arrives
/// everywhere at the planned speed; the window moves its braking points
/// for a car that is faster or slower than that, and for a wet or
/// rubbered stretch ahead. It lies on the lap's line's own points, so the
/// client recolours that line's dots and draws nothing new.
///
/// The forward pass starts at the car's speed, held to the corner it is
/// in (a car over the plan's corner speed is at its own limit, and a red
/// dot under it is no advice); the backward pass starts past the window,
/// at the lap's line's speed there.
#[derive(Debug, Clone)]
pub struct LineWindow {
    spacing: f32,
    points: Vec<[f32; 3]>,
    kappa: Vec<f32>,
    grade: Vec<f32>,
    drag_scale: Vec<f32>,
    /// Unit plan-view direction of the line at each point.
    tangent: Vec<[f32; 2]>,
    /// Each point's station along the centerline, m: the road state's.
    station_m: Vec<f32>,
    /// What the lap's line counts a hybrid's motor at (Balanced pacing).
    hybrid_share: Vec<f32>,
    /// The lap's line: its speeds (the window's far end) and phases.
    lap_mps: Vec<f32>,
    lap_phases: Vec<LinePhase>,
    /// The envelope of the last plan, and what it was made for.
    envelope: Option<(EnvelopeKey, CarEnvelope)>,
    /// Where the car was found last.
    hint: Option<usize>,
    /// Per point of the window, reused from plan to plan.
    road: Vec<f32>,
    corner: Vec<f32>,
    forward: Vec<f32>,
    backward: Vec<f32>,
    phases: Vec<LinePhase>,
}

/// What a window's envelope was made for: the fuel in whole litres, the
/// tyres' share in hundredths, the air and the track's grip in thousandths.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct EnvelopeKey {
    fuel_l: i32,
    tyres: i32,
    air: i32,
    grip: i32,
}

impl LineWindow {
    /// The window over `profile`, the lap's line for `car` on `track`.
    pub fn new(track: &TrackConfig, car: &CarConfig, profile: &RacingLineProfile) -> Self {
        let points = profile.points.clone();
        let n = points.len();
        let spacing = profile.spacing_m;
        let tangent = (0..n)
            .map(|i| {
                let (a, b) = (points[i], points[(i + 1) % n]);
                let (dx, dy) = (b[0] - a[0], b[1] - a[1]);
                let len = (dx * dx + dy * dy).sqrt().max(1e-6);
                [dx / len, dy / len]
            })
            .collect();
        let hybrid_share = match (
            car.hybrid.deploy_kj_per_lap,
            crate::hybrid::MotorPlan::new(car),
        ) {
            (Some(budget), Some(motor)) => paced_shares(profile, car, &motor, budget),
            _ => vec![1.0; n],
        };
        Self {
            spacing,
            kappa: curvature(&points, spacing),
            grade: grades(&points, spacing),
            drag_scale: drag_scales(track, car, n, spacing),
            station_m: centerline_stations(track, n, spacing),
            tangent,
            hybrid_share,
            lap_mps: profile.speed_mps.clone(),
            lap_phases: profile.phases.clone(),
            points,
            envelope: None,
            hint: None,
            road: Vec::new(),
            corner: Vec::new(),
            forward: Vec::new(),
            backward: Vec::new(),
            phases: Vec::new(),
        }
    }

    /// The lap's line's phases: what the client shows outside a window.
    pub fn lap_phases(&self) -> &[LinePhase] {
        &self.lap_phases
    }

    /// The point of the line nearest `pos` (server frame, m), searched
    /// near where the car was found last; `None` when the car is further
    /// than [`WINDOW_MAX_OFF_LINE_M`] from the line.
    pub fn locate(&mut self, pos: [f32; 3]) -> Option<usize> {
        let n = self.points.len();
        let max_d2 = WINDOW_MAX_OFF_LINE_M * WINDOW_MAX_OFF_LINE_M;
        let points = &self.points;
        let d2 = |i: usize| {
            let p = points[i];
            (p[0] - pos[0]).powi(2) + (p[1] - pos[1]).powi(2) + (p[2] - pos[2]).powi(2)
        };
        let near = self
            .hint
            .filter(|&h| h < n && n > 2 * LOCATE_REACH + 1)
            .and_then(|h| {
                let mut best = (h, d2(h), 0);
                for off in 1..=LOCATE_REACH {
                    for i in [(h + off) % n, (h + n - off) % n] {
                        let d = d2(i);
                        if d < best.1 {
                            best = (i, d, off);
                        }
                    }
                }
                // Best at the edge, or off the line: the car went further
                // than the reach (a reset, a rewind), so look everywhere.
                (best.2 < LOCATE_REACH && best.1 <= max_d2).then_some((best.0, best.1))
            });
        let (index, d) = near.unwrap_or_else(|| {
            (0..n)
                .map(|i| (i, d2(i)))
                .min_by(|a, b| a.1.total_cmp(&b.1))
                .unwrap_or((0, f32::MAX))
        });
        if d > max_d2 {
            self.hint = None;
            return None;
        }
        self.hint = Some(index);
        Some(index)
    }

    /// The phases of the window from point `start` on: `car` on `track`
    /// as it is now (its road state, its wind and air), carrying
    /// `fuel_liters`, on tyres worth `tyre_share` of the planned grip
    /// (`CarState::tyres_grip_share`, the road under the car left out), at
    /// `speed_mps`. [`WINDOW_AHEAD_M`] of them, or the lap less a point.
    pub fn plan(
        &mut self,
        track: &TrackConfig,
        car: &CarConfig,
        fuel_liters: f32,
        tyre_share: f32,
        start: usize,
        speed_mps: f32,
    ) -> &[LinePhase] {
        let n = self.points.len();
        let spacing = self.spacing;
        let shown = ((WINDOW_AHEAD_M / spacing).round() as usize).clamp(1, n - 1);
        let m = (((WINDOW_AHEAD_M + WINDOW_TAIL_M) / spacing).round() as usize).clamp(shown, n - 1);
        let start = start % n;
        let surface = &track.track_surface;
        let key = EnvelopeKey {
            fuel_l: fuel_liters.round() as i32,
            tyres: (tyre_share * 100.0).round() as i32,
            air: (surface.air_density_ratio * 1000.0).round() as i32,
            grip: (surface.base_grip * 1000.0).round() as i32,
        };
        if self.envelope.as_ref().is_none_or(|(k, _)| *k != key) {
            let (mass, _) = car.laden(fuel_liters);
            let envelope = CarEnvelope::new(
                car,
                surface.base_grip * tyre_share,
                mass,
                surface.air_density_ratio,
            );
            self.envelope = Some((key, envelope));
        }
        let Some((_, envelope)) = self.envelope.as_ref() else {
            return &[];
        };

        // The wind along the line at each point, behind the car positive:
        // the downforce is the airflow's.
        let wind = surface.wind_now_mps;
        let tangent = &self.tangent;
        let tail = |i: usize| wind[0] * tangent[i][0] + wind[1] * tangent[i][1];
        let road_state = track.road_state.as_ref();
        self.road.clear();
        self.corner.clear();
        for j in 0..m {
            let i = (start + j) % n;
            let road = road_state.map_or(1.0, |r| r.line_grip_at(self.station_m[i]));
            self.road.push(road);
            self.corner
                .push(envelope.corner_speed_in(self.kappa[i], road, tail(i)));
        }

        // The passes count the drag in still air with the flap shut: what
        // the wind along the line, and the flap, take off it.
        let drag_saved = |v: f32, i: usize, scale: f32| {
            let air = v - tail(i);
            envelope.drag_k * (v * v - scale * air * air.abs())
        };

        self.forward.clear();
        self.forward.push(speed_mps.max(0.0).min(self.corner[0]));
        for j in 1..m {
            let p = (start + j - 1) % n;
            let v = self.forward[j - 1];
            let a = envelope.acceleration_in(
                v,
                self.kappa[p],
                self.grade[p],
                self.hybrid_share[p],
                self.road[j - 1],
                tail(p),
            ) + drag_saved(v, p, self.drag_scale[p]);
            let reachable = (v * v + 2.0 * a * spacing).max(0.0).sqrt();
            self.forward.push(self.corner[j].min(reachable));
        }

        self.backward.clear();
        self.backward.resize(m, 0.0);
        self.backward[m - 1] = self.corner[m - 1].min(self.lap_mps[(start + m - 1) % n]);
        for j in (0..m - 1).rev() {
            let i = (start + j) % n;
            let v = self.backward[j + 1];
            // The flap is shut under braking.
            let d =
                envelope.deceleration_in(v, self.kappa[i], self.grade[i], self.road[j], tail(i))
                    - drag_saved(v, i, 1.0);
            let enterable = (v * v + 2.0 * d * spacing).max(0.0).sqrt();
            self.backward[j] = self.corner[j].min(enterable);
        }

        self.phases.clear();
        for j in 0..m {
            self.phases.push(phase_of(
                self.forward[j],
                self.backward[j],
                self.corner[j],
                envelope.top_speed,
            ));
            // The speed the car is planned at, for the lifts.
            self.forward[j] = self.forward[j].min(self.backward[j]);
        }
        downgrade_lifts(&mut self.phases, &self.forward, false);
        absorb_short_runs(
            &mut self.phases,
            (MIN_RUN_M / spacing).ceil() as usize,
            false,
        );
        &self.phases[..shown]
    }
}

/// The slope from each point of a closed, evenly spaced line to the next,
/// sin of it, positive uphill, held within ±0.3.
fn grades(points: &[[f32; 3]], spacing: f32) -> Vec<f32> {
    let n = points.len();
    (0..n)
        .map(|i| {
            let next = points[(i + 1) % n];
            ((next[2] - points[i][2]) / spacing).clamp(-0.3, 0.3)
        })
        .collect()
}

/// Where each of `n` points `spacing` apart along the line lies along the
/// centerline, m: the line is stationed along the raceline, the track's
/// zones and road along the centerline, and the two run the same lap, so
/// the station is scaled between them.
fn centerline_stations(track: &TrackConfig, n: usize, spacing: f32) -> Vec<f32> {
    let center_total = crate::laps::track_length_m(track);
    let line_total = spacing * n as f32;
    (0..n)
        .map(|i| i as f32 * spacing * center_total / line_total)
        .collect()
}

/// The car's drag at each of `n` points `spacing` apart against its
/// closed-flap drag: the flap-open drag inside a DRS zone, 1 elsewhere.
fn drag_scales(track: &TrackConfig, car: &CarConfig, n: usize, spacing: f32) -> Vec<f32> {
    let center_total = crate::laps::track_length_m(track);
    let line_total = spacing * n as f32;
    match (car.drs, center_total > 0.0 && line_total > 0.0) {
        (Some(drs), true) if !track.drs_zones.is_empty() => centerline_stations(track, n, spacing)
            .into_iter()
            .map(|station| {
                let open = track
                    .drs_zones
                    .iter()
                    .any(|z| crate::drs::inside(station, z.start_m, z.end_m, center_total));
                if open {
                    1.0 - drs.drag_reduction
                } else {
                    1.0
                }
            })
            .collect(),
        _ => vec![1.0; n],
    }
}

/// Fuel the car burns over one lap of `profile` carrying `fuel_liters`, in
/// litres: at every point the power the profile's speed change asks for
/// against drag, rolling and the grade, less what a hybrid adds, capped at
/// the engine's peak, burnt at the car's efficiency; idle where it asks
/// for none. An estimate: the profile's margins make it a little slower
/// than a car at the limit, so the real figure runs a few percent higher.
pub fn lap_fuel_liters(profile: &RacingLineProfile, car: &CarConfig, fuel_liters: f32) -> f32 {
    let n = profile.points.len();
    if n < 3 || profile.speed_mps.len() != n {
        return 0.0;
    }
    let (mass, _) = car.laden(fuel_liters);
    let drag_k = 0.5 * AIR_DENSITY * car.drag_coefficient * car.frontal_area_m2;
    let rolling_n = car.tire_config.rolling_resistance * GRAVITY * mass;
    let efficiency = car.transmission.efficiency.clamp(0.5, 1.0);
    let wide_open = wide_open_power_w(car);
    let spacing = profile.spacing_m.max(1e-3);
    let fuel = &car.fuel;
    let mut liters = 0.0;
    for i in 0..n {
        let next = (i + 1) % n;
        let (v0, v1) = (profile.speed_mps[i], profile.speed_mps[next]);
        let v = (0.5 * (v0 + v1)).max(1.0);
        let dt = spacing / v;
        let accel = (v1 * v1 - v0 * v0) / (2.0 * spacing);
        let grade = ((profile.points[next][2] - profile.points[i][2]) / spacing).clamp(-0.3, 0.3);
        let wheel_force = mass * (accel + GRAVITY * grade) + drag_k * v * v + rolling_n;
        let demand_w = (wheel_force * v / efficiency).max(0.0);
        // Where the plan is power-limited the throttle is wide open, and
        // the engine makes its curve whatever a hybrid adds on top; below
        // that (traction, a corner, a lift) it makes what is asked of it.
        let engine_w = if demand_w >= 0.95 * car.max_engine_power_w {
            wide_open
        } else {
            demand_w.min(wide_open)
        };
        let load_lps = match fuel.load_consumption_scale {
            // The old rule burns by throttle; the engine's share of its
            // peak stands in for it, near the top of the rev range.
            Some(scale) => engine_w / car.max_engine_power_w.max(1.0) * 0.85 * scale,
            None => fuel.burn_lps(engine_w),
        };
        liters += (fuel.idle_consumption_lps + load_lps) * dt;
    }
    liters
}

/// How long a lap of `profile` takes at its speeds, s: the ideal lap, a
/// little quicker than anyone drives it (the AI runs 10-15% over), which
/// is the safe side for a timed race's laps and fuel. 0 for a profile too
/// short to drive.
pub fn lap_time_s(profile: &RacingLineProfile) -> f32 {
    let n = profile.speed_mps.len();
    if n < 3 {
        return 0.0;
    }
    let spacing = profile.spacing_m.max(1e-3);
    (0..n)
        .map(|i| {
            let v = 0.5 * (profile.speed_mps[i] + profile.speed_mps[(i + 1) % n]);
            spacing / v.max(1.0)
        })
        .sum()
}

/// The engine's power flat out where a racing driver keeps it, W: its
/// curve averaged over the top quarter of the revs below the redline,
/// where the gearbox holds it between shifts.
fn wide_open_power_w(car: &CarConfig) -> f32 {
    const SAMPLES: usize = 16;
    let top = car.redline_rpm.max(car.idle_rpm + 1.0);
    let bottom = (top * 0.75).max(car.idle_rpm);
    let total: f32 = (0..SAMPLES)
        .map(|k| {
            let rpm = bottom + (top - bottom) * (k as f32 + 0.5) / SAMPLES as f32;
            crate::physics::engine_curve_torque_nm(car, rpm) * rpm * std::f32::consts::TAU / 60.0
        })
        .sum();
    (total / SAMPLES as f32).max(0.0)
}

/// Length of a closed polyline, including the closing segment.
fn loop_length(points: &[[f32; 3]]) -> f32 {
    (0..points.len())
        .map(|i| distance(points[i], points[(i + 1) % points.len()]))
        .sum()
}

fn distance(a: [f32; 3], b: [f32; 3]) -> f32 {
    let (dx, dy, dz) = (b[0] - a[0], b[1] - a[1], b[2] - a[2]);
    (dx * dx + dy * dy + dz * dz).sqrt()
}

/// Resample a closed polyline to evenly spaced points, as close to `spacing`
/// as divides the loop exactly.
fn resample_loop(source: &[[f32; 3]], spacing: f32) -> Option<Vec<[f32; 3]>> {
    // Drop repeated points: they make zero-length segments.
    let mut clean: Vec<[f32; 3]> = Vec::with_capacity(source.len());
    for &p in source {
        if clean.last().is_none_or(|&last| distance(last, p) > 1e-3) {
            clean.push(p);
        }
    }
    while clean.len() > 1 && distance(clean[0], clean[clean.len() - 1]) <= 1e-3 {
        clean.pop();
    }
    if clean.len() < 3 {
        return None;
    }

    let total = loop_length(&clean);
    let count = (total / spacing).round() as usize;
    if count < 3 {
        return None;
    }
    let step = total / count as f32;

    let mut out = Vec::with_capacity(count);
    let mut segment = 0usize;
    let mut segment_start = 0.0f32;
    for k in 0..count {
        let target = k as f32 * step;
        loop {
            let a = clean[segment];
            let b = clean[(segment + 1) % clean.len()];
            let len = distance(a, b);
            if target <= segment_start + len || segment + 1 == clean.len() {
                let t = ((target - segment_start) / len.max(1e-6)).clamp(0.0, 1.0);
                out.push([
                    a[0] + (b[0] - a[0]) * t,
                    a[1] + (b[1] - a[1]) * t,
                    a[2] + (b[2] - a[2]) * t,
                ]);
                break;
            }
            segment_start += len;
            segment += 1;
        }
    }
    Some(out)
}

/// Unsigned plan-view curvature at every point of a closed, evenly spaced
/// polyline: the circle through the points a half-chord either side, then a
/// light smoothing so one noisy point does not make a corner of its own.
fn curvature(points: &[[f32; 3]], spacing: f32) -> Vec<f32> {
    let n = points.len();
    let half = ((CURVATURE_HALF_CHORD_M / spacing).round() as usize).clamp(1, n / 3);
    let raw: Vec<f32> = (0..n)
        .map(|i| {
            let a = points[(i + n - half) % n];
            let b = points[i];
            let c = points[(i + half) % n];
            let ab = ((b[0] - a[0]).powi(2) + (b[1] - a[1]).powi(2)).sqrt();
            let bc = ((c[0] - b[0]).powi(2) + (c[1] - b[1]).powi(2)).sqrt();
            let ca = ((a[0] - c[0]).powi(2) + (a[1] - c[1]).powi(2)).sqrt();
            let cross = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
            let denominator = ab * bc * ca;
            if denominator <= 1e-6 {
                0.0
            } else {
                2.0 * cross.abs() / denominator
            }
        })
        .collect();

    // Triangular weights 1-2-3-2-1 over five points.
    const WEIGHTS: [f32; 5] = [1.0, 2.0, 3.0, 2.0, 1.0];
    (0..n)
        .map(|i| {
            let sum: f32 = WEIGHTS
                .iter()
                .enumerate()
                .map(|(j, w)| w * raw[(i + n + j - 2) % n])
                .sum();
            sum / 9.0
        })
        .collect()
}

/// What the driver does at a point the forward pass reaches at `forward`,
/// the backward pass lets in at `backward`, and the corner holds to
/// `corner`, for a car whose top speed is `top_speed`.
fn phase_of(forward: f32, backward: f32, corner: f32, top_speed: f32) -> LinePhase {
    if backward < forward - SPEED_EPSILON_MPS {
        LinePhase::Brake
    } else if forward < corner - SPEED_EPSILON_MPS || corner >= top_speed - SPEED_EPSILON_MPS {
        LinePhase::Throttle
    } else {
        LinePhase::Partial
    }
}

/// Contiguous runs of one phase as (start, length): around the loop from a
/// phase change when `closed` (empty when the whole loop is a single
/// phase), from the first point to the last when not.
fn runs_of(phases: &[LinePhase], closed: bool) -> Vec<(usize, usize)> {
    if closed {
        return runs(phases);
    }
    let mut out = Vec::new();
    let mut start = 0;
    for i in 1..=phases.len() {
        if i == phases.len() || phases[i] != phases[start] {
            out.push((start, i - start));
            start = i;
        }
    }
    out
}

/// Contiguous runs of one phase around the loop, as (start, length), starting
/// at a phase change. Empty when the whole loop is a single phase.
fn runs(phases: &[LinePhase]) -> Vec<(usize, usize)> {
    let n = phases.len();
    let Some(first) = (0..n).find(|&i| phases[i] != phases[(i + n - 1) % n]) else {
        return Vec::new();
    };
    let mut out = Vec::new();
    let mut start = first;
    let mut len = 0;
    for step in 0..n {
        let i = (first + step) % n;
        if step > 0 && phases[i] != phases[(i + n - 1) % n] {
            out.push((start, len));
            start = i;
            len = 0;
        }
        len += 1;
    }
    out.push((start, len));
    out
}

/// Turn braking zones that barely slow the car into partial throttle: a
/// fast kink wants a lift, and a brake marker there would teach a stab of
/// the pedal the corner does not need.
///
/// `closed`: the phases go round a lap; otherwise they are a stretch of
/// one, and a run at either end is cut off by it.
fn downgrade_lifts(phases: &mut [LinePhase], speed: &[f32], closed: bool) {
    let n = phases.len();
    for (start, len) in runs_of(phases, closed) {
        if phases[start] != LinePhase::Brake {
            continue;
        }
        let entry = speed[start];
        let exit = speed[(start + len - 1) % n];
        if entry - exit < LIFT_THRESHOLD_MPS {
            for k in 0..len {
                phases[(start + k) % n] = LinePhase::Partial;
            }
        }
    }
}

/// Merge runs shorter than `min_len` points into the run before them; on a
/// stretch (`closed` false) the first run has none and stays.
fn absorb_short_runs(phases: &mut [LinePhase], min_len: usize, closed: bool) {
    let n = phases.len();
    // Each merge can create a new, longer run next to it, so repeat until
    // nothing changes; a handful of passes at most.
    for _ in 0..8 {
        let mut changed = false;
        for (start, len) in runs_of(phases, closed) {
            if len >= min_len || (!closed && start == 0) {
                continue;
            }
            let before = phases[(start + n - 1) % n];
            if phases[start] != before {
                for k in 0..len {
                    phases[(start + k) % n] = before;
                }
                changed = true;
            }
        }
        if !changed {
            break;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::data::{RacelinePoint, TrackConfig};

    /// A stadium: two straights joined by semicircles of `radius`.
    fn stadium(straight: f32, radius: f32) -> TrackConfig {
        let mut raceline = Vec::new();
        let step = 2.0;
        let mut push = |x: f32, y: f32| raceline.push(RacelinePoint { x, y, z: 0.0 });
        let arc_steps = ((std::f32::consts::PI * radius) / step).ceil() as usize;
        let straight_steps = (straight / step).ceil() as usize;
        for i in 0..straight_steps {
            push(i as f32 * straight / straight_steps as f32, 0.0);
        }
        for i in 0..arc_steps {
            let a =
                -std::f32::consts::FRAC_PI_2 + std::f32::consts::PI * i as f32 / arc_steps as f32;
            push(straight + radius * a.cos(), radius + radius * a.sin());
        }
        for i in 0..straight_steps {
            push(
                straight - i as f32 * straight / straight_steps as f32,
                2.0 * radius,
            );
        }
        for i in 0..arc_steps {
            let a =
                std::f32::consts::FRAC_PI_2 + std::f32::consts::PI * i as f32 / arc_steps as f32;
            push(radius * a.cos(), radius + radius * a.sin());
        }
        let mut track = TrackConfig {
            raceline,
            ..TrackConfig::default()
        };
        track.rebuild_raceline_distances();
        track
    }

    fn phase_at(profile: &RacingLineProfile, x: f32, y: f32) -> LinePhase {
        let nearest = (0..profile.points.len())
            .min_by(|&a, &b| {
                let da = (profile.points[a][0] - x).powi(2) + (profile.points[a][1] - y).powi(2);
                let db = (profile.points[b][0] - x).powi(2) + (profile.points[b][1] - y).powi(2);
                da.total_cmp(&db)
            })
            .unwrap();
        profile.phases[nearest]
    }

    #[test]
    fn points_are_evenly_spaced_around_the_loop() {
        let track = stadium(600.0, 40.0);
        let profile = build(&track, &CarConfig::default()).unwrap();
        let n = profile.points.len();
        assert_eq!(profile.phases.len(), n);
        assert_eq!(profile.speed_mps.len(), n);
        for i in 0..n {
            let gap = distance(profile.points[i], profile.points[(i + 1) % n]);
            // Chords of the arcs come out a hair short of the arc spacing.
            assert!(
                (gap - profile.spacing_m).abs() < 0.05,
                "gap {gap} at {i} vs spacing {}",
                profile.spacing_m
            );
        }
    }

    #[test]
    fn brakes_before_a_corner_and_accelerates_out_of_it() {
        let track = stadium(800.0, 30.0);
        let profile = build(&track, &CarConfig::default()).unwrap();

        // Early on the straight the car is still accelerating out of the
        // previous corner; at its end it is braking for the next one.
        assert_eq!(phase_at(&profile, 60.0, 0.0), LinePhase::Throttle);
        assert_eq!(phase_at(&profile, 790.0, 0.0), LinePhase::Brake);
        // Mid-corner it is held at the limit.
        assert_eq!(phase_at(&profile, 830.0, 30.0), LinePhase::Partial);

        // The corner speed is what the default car's grip allows around a
        // 30 m radius, less the margin.
        let slowest = profile.speed_mps.iter().copied().fold(f32::MAX, f32::min);
        let envelope = CarEnvelope::new(
            &CarConfig::default(),
            1.0,
            CarConfig::default().mass_kg,
            1.0,
        );
        let expected = envelope.corner_speed(1.0 / 30.0, 1.0);
        assert!(
            (slowest - expected).abs() < 1.0,
            "slowest {slowest} vs corner speed {expected}"
        );
    }

    #[test]
    fn braking_zone_is_as_long_as_the_brakes_need() {
        let track = stadium(1200.0, 30.0);
        let car = CarConfig::default();
        let profile = build(&track, &car).unwrap();
        let brake_points = profile
            .phases
            .iter()
            .filter(|&&p| p == LinePhase::Brake)
            .count();
        // Two corners, so two braking zones of equal length.
        let zone_m = brake_points as f32 * profile.spacing_m / 2.0;
        let fastest = profile.speed_mps.iter().copied().fold(0.0, f32::max);
        let slowest = profile.speed_mps.iter().copied().fold(f32::MAX, f32::min);
        // Bounds from constant decelerations either side of what the car
        // does: tyres alone at their margin, and tyres plus drag at the top.
        let envelope = CarEnvelope::new(&car, 1.0, car.mass_kg, 1.0);
        let weakest = envelope.grip_accel(0.0, 1.0) * BRAKING_GRIP_MARGIN;
        let strongest = envelope.deceleration(fastest, 0.0, 0.0, 1.0);
        let longest = (fastest * fastest - slowest * slowest) / (2.0 * weakest);
        let shortest = (fastest * fastest - slowest * slowest) / (2.0 * strongest);
        assert!(
            zone_m > shortest - 5.0 && zone_m < longest + 5.0,
            "braking zone {zone_m} m outside {shortest}..{longest} m"
        );
    }

    /// A front-axle motor on an RWD car puts its power down through the
    /// fronts: where the rears are at their grip it still adds drive, which
    /// a crank motor through the same rears cannot. Where the engine alone
    /// is short of the grip, both count the same power.
    #[test]
    fn a_front_motor_counts_the_front_tyres_traction() {
        let mut crank = CarConfig {
            drivetrain: Drivetrain::RWD,
            ..Default::default()
        };
        crank.hybrid.enabled = true;
        crank.hybrid.motor_max_power_kw = 150.0;
        crank.hybrid.battery_max_discharge_kw = 150.0;
        crank.hybrid.motor_max_torque_nm = 1000.0;
        let mut front = crank.clone();
        front.hybrid.axle = HybridAxle::Front;
        let at = |car: &CarConfig, grip: f32, speed: f32| {
            CarEnvelope::new(car, grip, car.mass_kg, 1.0).acceleration(speed, 0.0, 0.0, 1.0, 1.0)
        };
        // A wet road at low speed: the rears are the limit.
        let (c, f) = (at(&crank, 0.4, 15.0), at(&front, 0.4, 15.0));
        assert!(f > c + 0.5, "front {f:.2} vs crank {c:.2} m/s²");
        // Fast on a dry road the power is the limit: the same either way.
        let (c, f) = (at(&crank, 1.0, 70.0), at(&front, 1.0, 70.0));
        assert!((f - c).abs() < 1e-4, "front {f:.4} vs crank {c:.4} m/s²");
        // An AWD car's motor shares the engine's driven wheels: no change.
        crank.drivetrain = Drivetrain::AWD;
        front.drivetrain = Drivetrain::AWD;
        assert_eq!(at(&crank, 0.4, 15.0), at(&front, 0.4, 15.0));
    }

    #[test]
    fn the_plan_counts_the_motor_only_as_far_as_the_lap_budget_pays() {
        let track = stadium(800.0, 30.0);
        let plain = CarConfig::default();
        let mut hybrid = CarConfig::default();
        hybrid.hybrid.enabled = true;
        hybrid.hybrid.motor_max_power_kw = 150.0;
        hybrid.hybrid.battery_max_discharge_kw = 150.0;
        hybrid.hybrid.motor_max_torque_nm = 1000.0;
        let full = build(&track, &hybrid).unwrap();
        let top = |p: &RacingLineProfile| p.speed_mps.iter().copied().fold(0.0, f32::max);
        // A budget the lap never reaches plans as no budget at all.
        hybrid.hybrid.deploy_kj_per_lap = Some(1.0e6);
        assert_eq!(build(&track, &hybrid).unwrap().speed_mps, full.speed_mps);
        // A tight one costs speed and time, but not all of the motor.
        hybrid.hybrid.deploy_kj_per_lap = Some(400.0);
        let paced = build(&track, &hybrid).unwrap();
        let none = build(&track, &plain).unwrap();
        println!(
            "lap {:.2} s at full power, {:.2} s on 400 kJ, {:.2} s without; top {:.1} / {:.1} / {:.1} m/s",
            lap_time_s(&full),
            lap_time_s(&paced),
            lap_time_s(&none),
            top(&full),
            top(&paced),
            top(&none)
        );
        assert!(lap_time_s(&paced) > lap_time_s(&full) + 0.1);
        assert!(lap_time_s(&paced) < lap_time_s(&none) - 0.1);
        let motor = crate::hybrid::MotorPlan::new(&hybrid).unwrap();
        let shares = paced_shares(&paced, &hybrid, &motor, 400.0);
        assert!(
            shares.iter().any(|s| *s < 0.99),
            "the budget binds somewhere"
        );
        // A motor that drives only above a speed the car never reaches is
        // no motor to the plan.
        hybrid.hybrid.deploy_kj_per_lap = None;
        hybrid.hybrid.deploy_min_speed_kph = 1000.0;
        assert_eq!(build(&track, &hybrid).unwrap().speed_mps, none.speed_mps);
    }

    /// The AI's plan counts the motor as the aid does (paced over the lap
    /// budget, nothing below its minimum speed), and carries the plan with
    /// the motor unpaced beside it for the AI's cap.
    #[test]
    fn the_ai_plans_the_motor_as_it_drives_and_caps_on_it_unpaced() {
        let track = stadium(800.0, 30.0);
        let mut hybrid = CarConfig::default();
        hybrid.hybrid.enabled = true;
        hybrid.hybrid.motor_max_power_kw = 150.0;
        hybrid.hybrid.battery_max_discharge_kw = 150.0;
        hybrid.hybrid.motor_max_torque_nm = 1000.0;
        hybrid.hybrid.deploy_kj_per_lap = Some(400.0);
        let laden = build_laden(&track, &hybrid, 0.0).unwrap();
        assert_eq!(laden.speed_mps, build(&track, &hybrid).unwrap().speed_mps);
        let mut unpaced = hybrid.clone();
        unpaced.hybrid.deploy_kj_per_lap = None;
        let full = build(&track, &unpaced).unwrap();
        assert_eq!(laden.unpaced_mps, full.speed_mps);
        assert_eq!(full.unpaced_mps, full.speed_mps);
        assert!(lap_time_s(&full) < lap_time_s(&laden) - 0.1);
        // A motor that may drive only above a speed the car never reaches
        // is no motor to the AI either.
        hybrid.hybrid.deploy_min_speed_kph = 1000.0;
        let none = build(&track, &CarConfig::default()).unwrap();
        let wec = build_laden(&track, &hybrid, 0.0).unwrap();
        assert_eq!(wec.speed_mps, none.speed_mps);
        assert_eq!(wec.unpaced_mps, none.speed_mps);
    }

    #[test]
    fn more_grip_carries_more_speed_through_corners() {
        let track = stadium(500.0, 40.0);
        let slow = CarConfig::default();
        let mut fast = CarConfig::default();
        fast.tire_config.grip_coefficient = 1.6;
        let min_speed = |car: &CarConfig| {
            build(&track, car)
                .unwrap()
                .speed_mps
                .into_iter()
                .fold(f32::MAX, f32::min)
        };
        assert!(min_speed(&fast) > min_speed(&slow) + 3.0);
    }

    #[test]
    fn a_gentle_kink_is_a_lift_not_a_brake() {
        let mut phases = vec![LinePhase::Throttle; 40];
        let mut speed = vec![80.0; 40];
        for (i, p) in phases.iter_mut().enumerate().skip(10).take(6) {
            *p = LinePhase::Brake;
            speed[i] = 80.0 - (i - 10) as f32 * 0.3;
        }
        downgrade_lifts(&mut phases, &speed, true);
        assert!(phases.iter().all(|&p| p != LinePhase::Brake));
    }

    #[test]
    fn short_runs_are_absorbed() {
        use LinePhase::*;
        let mut phases = vec![
            Throttle, Throttle, Throttle, Partial, Brake, Brake, Brake, Brake,
        ];
        absorb_short_runs(&mut phases, 2, true);
        // The single Partial point takes the phase of the run before it.
        assert_eq!(phases[3], Throttle);
    }

    #[test]
    fn falls_back_to_the_centerline() {
        let track = TrackConfig::default();
        assert!(track.raceline.is_empty());
        let profile = build(&track, &CarConfig::default()).unwrap();
        assert!(profile.points.len() > 100);
    }

    /// A window over the stadium's first straight, from 100 m into it (the
    /// line runs +x from its first point): the line, the window, its start
    /// and where the lap's own line starts braking from there, in points.
    fn straight_window() -> (
        TrackConfig,
        CarConfig,
        RacingLineProfile,
        LineWindow,
        usize,
        usize,
    ) {
        let track = stadium(800.0, 30.0);
        let car = CarConfig::default();
        let profile = build(&track, &car).unwrap();
        let window = LineWindow::new(&track, &car, &profile);
        let start = (100.0 / profile.spacing_m).round() as usize;
        let lap_brake =
            first_brake(&profile.phases[start..]).expect("the lap brakes for the corner");
        (track, car, profile, window, start, lap_brake)
    }

    fn first_brake(phases: &[LinePhase]) -> Option<usize> {
        phases.iter().position(|&p| p == LinePhase::Brake)
    }

    #[test]
    fn a_window_on_the_plan_brakes_where_the_lap_does() {
        let (track, car, profile, mut window, start, lap_brake) = straight_window();
        let phases = window.plan(&track, &car, 0.0, 1.0, start, profile.speed_mps[start]);
        assert_eq!(
            phases.len(),
            (WINDOW_AHEAD_M / profile.spacing_m).round() as usize
        );
        let brake = first_brake(phases).expect("the window brakes for the corner");
        assert!(
            brake.abs_diff(lap_brake) <= 1,
            "window brakes at {brake}, the lap at {lap_brake}"
        );
    }

    #[test]
    fn a_window_moves_the_braking_point_with_the_car_s_speed_and_grip() {
        let (track, car, profile, mut window, start, lap_brake) = straight_window();
        let planned = profile.speed_mps[start];
        let at = |window: &mut LineWindow, speed: f32, tyres: f32| {
            first_brake(window.plan(&track, &car, 0.0, tyres, start, speed)).unwrap()
        };
        let fast = at(&mut window, planned + 10.0, 1.0);
        let slow = at(&mut window, planned - 10.0, 1.0);
        let worn = at(&mut window, planned, 0.7);
        println!("lap {lap_brake}, fast {fast}, slow {slow}, worn {worn}");
        assert!(
            fast < lap_brake,
            "faster brakes earlier: {fast} vs {lap_brake}"
        );
        assert!(
            slow > lap_brake,
            "slower brakes later: {slow} vs {lap_brake}"
        );
        assert!(
            worn < lap_brake,
            "less grip brakes earlier: {worn} vs {lap_brake}"
        );
    }

    #[test]
    fn a_headwind_moves_the_braking_point_later() {
        let (mut track, car, profile, mut window, start, lap_brake) = straight_window();
        track.track_surface.wind_now_mps = [-15.0, 0.0];
        let phases = window.plan(&track, &car, 0.0, 1.0, start, profile.speed_mps[start]);
        let brake = first_brake(phases).unwrap();
        assert!(brake > lap_brake, "into the wind {brake} vs {lap_brake}");
    }

    /// The wind in the downforce: into it, a downforce car's corner is
    /// quicker than in still air, with it slower; a car without downforce
    /// corners as in still air.
    #[test]
    fn a_headwind_loads_the_downforce_in_a_corner() {
        let mut car = CarConfig {
            lift_coefficient_front: 0.0,
            lift_coefficient_rear: 0.0,
            ..Default::default()
        };
        let envelope = |car: &CarConfig| CarEnvelope::new(car, 1.0, 1000.0, 1.0);
        let kappa = 1.0 / 120.0;
        let none = envelope(&car);
        assert!(
            (none.corner_speed_in(kappa, 1.0, -10.0) - none.corner_speed(kappa, 1.0)).abs() < 1e-3
        );
        car.lift_coefficient_front = -1.2;
        car.lift_coefficient_rear = -1.6;
        let winged = envelope(&car);
        let still = winged.corner_speed_in(kappa, 1.0, 0.0);
        assert_eq!(
            still,
            winged.corner_speed(kappa, 1.0),
            "still air to the bit"
        );
        let into = winged.corner_speed_in(kappa, 1.0, -10.0);
        let with = winged.corner_speed_in(kappa, 1.0, 10.0);
        println!("corner at 120 m: still {still:.2}, into {into:.2}, with {with:.2} m/s");
        assert!(into > still + 0.3 && with < still - 0.3);
        // The answer holds the corner: the grip the airflow gives.
        let grip = winged.grip_accel_in(into, 1.0, -10.0) * LATERAL_GRIP_MARGIN;
        assert!((kappa * into * into - grip).abs() < 0.05 * grip);
    }

    #[test]
    fn a_window_finds_the_car_on_the_line_and_not_off_it() {
        let (_, _, profile, mut window, start, _) = straight_window();
        let [x, y, z] = profile.points[start];
        assert_eq!(window.locate([x + 0.4, y + 3.0, z]), Some(start));
        // The next search starts from there, and still finds the car on.
        let ahead = profile.points[start + 8];
        assert_eq!(window.locate(ahead), Some(start + 8));
        assert_eq!(window.locate([x, y - 2.0 * WINDOW_MAX_OFF_LINE_M, z]), None);
    }
}
