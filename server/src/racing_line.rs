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
//! A driving aid, not part of the simulation: nothing here feeds physics or
//! the AI. It is computed once when a player joins a session and sent to
//! them as `ServerMessage::RacingLine`.

use crate::data::{CarConfig, Drivetrain, TrackConfig};
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
}

/// The car as a point mass: what limits its speed along a line.
struct CarEnvelope {
    /// Tyre grip times track grip.
    mu: f32,
    /// Downforce per unit mass per (m/s)², so downforce accel = k · v².
    downforce_k: f32,
    /// Drag per unit mass per (m/s)².
    drag_k: f32,
    rolling_accel: f32,
    /// Peak power at the wheels over mass, W/kg.
    power_per_kg: f32,
    /// Share of the car's weight on the driven wheels.
    driven_fraction: f32,
    /// Most the brakes can do on their own, m/s².
    brake_accel: f32,
    /// Speed at the limiter in top gear.
    top_speed: f32,
}

impl CarEnvelope {
    fn new(car: &CarConfig, track_grip: f32) -> Self {
        let mass = car.mass_kg.max(1.0);
        let lift = car.lift_coefficient_front + car.lift_coefficient_rear;
        let hybrid_w = if car.hybrid.enabled {
            car.hybrid.motor_max_power_kw * 1000.0
        } else {
            0.0
        };
        let driven_fraction = match car.drivetrain {
            Drivetrain::AWD => 1.0,
            Drivetrain::RWD => 1.0 - car.weight_distribution_front,
            Drivetrain::FWD => car.weight_distribution_front,
        };
        Self {
            mu: car.tire_config.grip_coefficient * track_grip,
            downforce_k: 0.5 * AIR_DENSITY * car.frontal_area_m2 * (-lift).max(0.0) / mass,
            drag_k: 0.5 * AIR_DENSITY * car.drag_coefficient * car.frontal_area_m2 / mass,
            rolling_accel: car.tire_config.rolling_resistance * GRAVITY,
            // Peak power over the whole run: a real gearbox spends part of
            // each gear below the peak, so this reaches a little more speed
            // than the car does, which errs towards braking early.
            power_per_kg: (car.max_engine_power_w * car.transmission.efficiency + hybrid_w) / mass,
            driven_fraction: driven_fraction.clamp(0.2, 1.0),
            brake_accel: car.max_brake_force_n / mass,
            top_speed: top_gear_speed(car),
        }
    }

    /// Total grip the tyres offer at `speed`, m/s².
    fn grip_accel(&self, speed: f32) -> f32 {
        self.mu * (GRAVITY + self.downforce_k * speed * speed)
    }

    /// Fastest speed a corner of curvature `kappa` can be taken at, capped at
    /// top speed. Solves κ·v² = μ·m·(g + k·v²) for v.
    fn corner_speed(&self, kappa: f32) -> f32 {
        let mu = self.mu * LATERAL_GRIP_MARGIN;
        let denominator = kappa - mu * self.downforce_k;
        if denominator <= 1e-6 {
            return self.top_speed;
        }
        (mu * GRAVITY / denominator).sqrt().min(self.top_speed)
    }

    /// Grip left along the car once `kappa` at `speed` has taken its share
    /// sideways (the friction circle).
    fn longitudinal_grip(&self, speed: f32, kappa: f32) -> f32 {
        let total = self.grip_accel(speed);
        let lateral = kappa * speed * speed;
        let used = (lateral / total.max(1e-3)).min(1.0);
        total * (1.0 - used * used).sqrt()
    }

    /// Net forward acceleration at full throttle; `grade` is sin of the
    /// slope, positive uphill.
    fn acceleration(&self, speed: f32, kappa: f32, grade: f32) -> f32 {
        let traction = self.longitudinal_grip(speed, kappa) * self.driven_fraction;
        let power = self.power_per_kg / speed.max(5.0);
        power.min(traction) - self.drag_k * speed * speed - self.rolling_accel - GRAVITY * grade
    }

    /// Deceleration at the braking limit (positive); `grade` as above.
    fn deceleration(&self, speed: f32, kappa: f32, grade: f32) -> f32 {
        let tyres = self.longitudinal_grip(speed, kappa) * BRAKING_GRIP_MARGIN;
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

/// Build the racing line `car` should follow on `track`, or `None` when the
/// track has too few points to make a loop.
pub fn build(track: &TrackConfig, car: &CarConfig) -> Option<RacingLineProfile> {
    let source: Vec<[f32; 3]> = if track.raceline.len() >= 3 {
        track.raceline.iter().map(|p| [p.x, p.y, p.z]).collect()
    } else {
        track.centerline.iter().map(|p| [p.x, p.y, p.z]).collect()
    };
    let points = resample_loop(&source, SAMPLE_SPACING_M)?;
    let n = points.len();
    let spacing = loop_length(&points) / n as f32;

    let envelope = CarEnvelope::new(car, track.track_surface.base_grip);
    let kappa = curvature(&points, spacing);
    let grade: Vec<f32> = (0..n)
        .map(|i| {
            let next = points[(i + 1) % n];
            ((next[2] - points[i][2]) / spacing).clamp(-0.3, 0.3)
        })
        .collect();

    let corner: Vec<f32> = kappa.iter().map(|&k| envelope.corner_speed(k)).collect();

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
        let a = envelope.acceleration(v, kappa[prev], grade[prev]);
        let reachable = (v * v + 2.0 * a * spacing).max(0.0).sqrt();
        forward[i] = forward[i].min(reachable);
    }

    let mut backward = corner.clone();
    for step in 1..n {
        let i = (slowest + n - step) % n;
        let next = (i + 1) % n;
        let v = backward[next];
        let d = envelope.deceleration(v, kappa[i], grade[i]);
        let enterable = (v * v + 2.0 * d * spacing).max(0.0).sqrt();
        backward[i] = backward[i].min(enterable);
    }

    let speed: Vec<f32> = (0..n).map(|i| forward[i].min(backward[i])).collect();
    let mut phases: Vec<LinePhase> = (0..n)
        .map(|i| {
            if backward[i] < forward[i] - SPEED_EPSILON_MPS {
                LinePhase::Brake
            } else if forward[i] < corner[i] - SPEED_EPSILON_MPS
                || corner[i] >= envelope.top_speed - SPEED_EPSILON_MPS
            {
                LinePhase::Throttle
            } else {
                LinePhase::Partial
            }
        })
        .collect();

    downgrade_lifts(&mut phases, &speed);
    absorb_short_runs(&mut phases, (MIN_RUN_M / spacing).ceil() as usize);

    Some(RacingLineProfile {
        spacing_m: spacing,
        points,
        speed_mps: speed,
        phases,
    })
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
fn downgrade_lifts(phases: &mut [LinePhase], speed: &[f32]) {
    let n = phases.len();
    for (start, len) in runs(phases) {
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

/// Merge runs shorter than `min_len` points into the run before them.
fn absorb_short_runs(phases: &mut [LinePhase], min_len: usize) {
    let n = phases.len();
    // Each merge can create a new, longer run next to it, so repeat until
    // nothing changes; a handful of passes at most.
    for _ in 0..8 {
        let mut changed = false;
        for (start, len) in runs(phases) {
            if len >= min_len {
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
        let envelope = CarEnvelope::new(&CarConfig::default(), 1.0);
        let expected = envelope.corner_speed(1.0 / 30.0);
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
        let envelope = CarEnvelope::new(&car, 1.0);
        let weakest = envelope.grip_accel(0.0) * BRAKING_GRIP_MARGIN;
        let strongest = envelope.deceleration(fastest, 0.0, 0.0);
        let longest = (fastest * fastest - slowest * slowest) / (2.0 * weakest);
        let shortest = (fastest * fastest - slowest * slowest) / (2.0 * strongest);
        assert!(
            zone_m > shortest - 5.0 && zone_m < longest + 5.0,
            "braking zone {zone_m} m outside {shortest}..{longest} m"
        );
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
        downgrade_lifts(&mut phases, &speed);
        assert!(phases.iter().all(|&p| p != LinePhase::Brake));
    }

    #[test]
    fn short_runs_are_absorbed() {
        use LinePhase::*;
        let mut phases = vec![
            Throttle, Throttle, Throttle, Partial, Brake, Brake, Brake, Brake,
        ];
        absorb_short_runs(&mut phases, 2);
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
}
