//! Hybrid deployment: when the electric motor drives, when it charges, and
//! how much of a lap's energy it may spend.
//!
//! A hybrid car (`[hybrid]` in car.toml) has a battery
//! (`CarState::hybrid_battery_kwh`), a motor that adds torque at the crank
//! and recovers it under braking, and, since this module, a driver's say
//! in how it is used:
//!
//! - a **mode** (`ErsMode`, `PlayerInput.ers_mode`): *Harvest* never
//!   deploys and charges from the engine on the throttle as well;
//!   *Balanced* (the default, and the AI's but when it harvests alone:
//!   [`ai_ers`]) deploys at full throttle,
//!   eased back smoothly when it is spending the lap's budget faster than
//!   the lap goes by, and eased out as the battery runs low; *Attack* deploys whenever the throttle is open until the budget
//!   or the battery is gone;
//! - an **overtake button** (`PlayerInput.ers_boost`, held): *Attack*
//!   whatever the mode, for as long as it is held;
//! - a **lap budget** (`[hybrid] deploy_kj_per_lap`, none by default): the
//!   energy the motor may deploy between two crossings of the line, as the
//!   F1 rules have it; the pacing reads it against the car's progress
//!   round the lap;
//! - a **minimum deployment speed** (`deploy_min_speed_kph`): the WEC's
//!   rule for a hypercar's motor, which may only drive above 190 km/h;
//! - **heat recovery** (`heat_recovery_kw`): a turbo's generator (an
//!   MGU-H) charging the battery while the engine works at full throttle,
//!   at no cost to its torque. No shipped car has one; an AC import of a
//!   2014-2020 F1 does;
//! - a **stint budget** (`stint_kj`): the energy the motor may deploy
//!   between two pit stops (the WEC's rule), reset by a stop, a grid or
//!   the garage ([`new_stint`]);
//! - a **deploy map** (`deploy_early`, the setup's, never the car.toml's):
//!   Balanced mode paces the budget against the share of the lap left;
//!   the map bends that share (`lap_share ^ (1 - deploy_early)`), so a
//!   positive map spends the budget earlier in the lap and a negative one
//!   saves it for the end;
//! - a **manual override allowance** (`override_kj_per_lap`): what the
//!   overtake button may spend per lap over and above the paced lap
//!   budget; with it the button draws on its own allowance and leaves the
//!   budget alone, and once it is spent the button does nothing more that
//!   lap. Without it the button is Attack on the lap budget, as before;
//! - an **axle** (`axle`, [`HybridAxle`]): the crank by default, or the
//!   front axle directly, a hypercar's motor turning with the front wheels
//!   ([`MotorOutput::front_axle_torque_nm`]): an RWD car drives at both
//!   ends while it deploys, and recovers, coasts and harvests on the
//!   front. Its `motor_max_torque_nm` is at the axle.
//!
//! Recovery: under braking the motor charges with the energy its axle's
//! tyres (the driven axle's for a crank motor, the fronts' for a front
//! one) are actually taking out of the car, up to the regen power
//! ([`MotorOutput::regen_w`]: last tick's braking power on that axle,
//! [`REGEN_AXLE_SHARE`] of it), and with **brake-by-wire** (`brake_by_wire`,
//! the default) that recovery stands in for the axle's hydraulic brakes,
//! so the pedal decelerates the car the same whether the battery takes
//! the energy or the discs do, and the discs run cooler for it; without
//! it the recovery brakes the driven axle on top of the pads. A car with
//! `bbw_response_s` has hydraulics that follow the recovery with that
//! time constant ([`bbw_offset_n`]), so the balance migrates while they
//! catch up: rearward as the recovery comes in, forward as a filling
//! battery takes it away. Off the
//! throttle and off the brake it charges at [`COAST_SHARE`] of the regen
//! power against the crank (a little more engine braking), and in Harvest
//! mode on the throttle at [`HARVEST_SHARE`] of it, again against the
//! crank.
//!
//! Telemetry carries the charge, the lap budget left and the mode and what
//! the motor is doing ([`telemetry_bytes`]).

use std::f32::consts::PI;

use crate::data::{CarConfig, CarState, HybridAxle, PlayerInputData};

/// What the driver asked the motor to do.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ErsMode {
    /// Never deploy; charge on the throttle too.
    Harvest = 0,
    /// Deploy at full throttle, paced over the lap.
    Balanced = 1,
    /// Deploy at any throttle until the energy is gone.
    Attack = 2,
}

impl ErsMode {
    /// From the wire's byte; anything unknown is Balanced.
    pub fn from_u8(v: u8) -> Self {
        match v {
            0 => ErsMode::Harvest,
            2 => ErsMode::Attack,
            _ => ErsMode::Balanced,
        }
    }
}

/// An AI chasing a car closer than this presses the overtake button, s.
pub const AI_BOOST_GAP_S: f32 = 0.8;

/// An AI with a car closer than this behind presses the overtake button to
/// defend, s.
pub const AI_DEFEND_BOOST_GAP_S: f32 = 0.5;
/// An AI with the battery under this share of its capacity, nobody within
/// [`AI_ALONE_GAP_S`] ahead or behind, harvests on purpose: it gives up the
/// motor while alone so it has it in the fight.
pub const AI_HARVEST_BELOW: f32 = 0.3;
pub const AI_ALONE_GAP_S: f32 = 2.0;

/// Chasing a car, an AI presses the overtake button only on a straight
/// (or out of the corner onto one) at least this share of the lap's
/// longest: it saves the button's energy for where a pass is made, not
/// the short straights between.
pub const AI_ATTACK_STRAIGHT_SHARE: f32 = 0.6;

/// The full-throttle straights of a car's speed profile
/// ([`straights`]): per profile point, the length of the straight it is
/// on (a `Throttle` run), or out of a corner the next one; and the
/// longest of the lap.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct Straights {
    pub run_m: Vec<f32>,
    pub longest_m: f32,
}

impl Straights {
    /// The straight at profile point `here` (or the next one) as a share
    /// of the lap's longest; `None` for a point off the profile.
    pub fn share_at(&self, here: usize) -> Option<f32> {
        let run = *self.run_m.get(here)?;
        (self.longest_m > 0.0).then(|| run / self.longest_m)
    }
}

/// The straights of `profile` (see [`Straights`]), the loop wrapping.
pub fn straights(profile: &crate::racing_line::RacingLineProfile) -> Straights {
    use crate::racing_line::LinePhase;
    let n = profile.phases.len();
    let sp = profile.spacing_m.max(0.0);
    let flat = |i: usize| profile.phases[i] == LinePhase::Throttle;
    if n == 0 {
        return Straights::default();
    }
    if (0..n).all(flat) {
        return Straights {
            run_m: vec![n as f32 * sp; n],
            longest_m: n as f32 * sp,
        };
    }
    // Start at a point off the throttle, so no run is cut by the loop's
    // seam.
    let start = (0..n).find(|&i| !flat(i)).unwrap_or(0);
    let at = |k: usize| (start + k) % n;
    let mut run_m = vec![0.0; n];
    let mut longest_m: f32 = 0.0;
    let mut k = 1;
    while k < n {
        if !flat(at(k)) {
            k += 1;
            continue;
        }
        let mut len = 0;
        while k + len < n && flat(at(k + len)) {
            len += 1;
        }
        let m = len as f32 * sp;
        longest_m = longest_m.max(m);
        for j in 0..len {
            run_m[at(k + j)] = m;
        }
        k += len;
    }
    // Walking back round the loop, a corner takes the straight after it.
    let mut next = 0.0;
    for k in (0..2 * n).rev() {
        let i = at(k);
        if flat(i) {
            next = run_m[i];
        } else {
            run_m[i] = next;
        }
    }
    Straights { run_m, longest_m }
}

/// The mode a car starts in and the AI drives in.
pub const DEFAULT_MODE: u8 = ErsMode::Balanced as u8;

/// The AI's mode and overtake button in a race, past its first lap:
/// `charge` the battery's share, the gaps to the nearest cars ahead and
/// behind on the road, s, and the straight it is on or coming onto as a
/// share of the lap's longest ([`Straights::share_at`], `None` unknown).
/// The button to attack within [`AI_BOOST_GAP_S`] of the car ahead, on a
/// straight of [`AI_ATTACK_STRAIGHT_SHARE`] or more, and to defend within
/// [`AI_DEFEND_BOOST_GAP_S`] of the car behind anywhere; Harvest when low
/// and alone; Balanced otherwise.
pub fn ai_ers(
    charge: f32,
    gap_ahead_s: Option<f32>,
    gap_behind_s: Option<f32>,
    straight_share: Option<f32>,
) -> (ErsMode, bool) {
    let within = |gap: Option<f32>, s: f32| gap.is_some_and(|g| g <= s);
    let attack_here = straight_share.is_none_or(|share| share >= AI_ATTACK_STRAIGHT_SHARE);
    if (within(gap_ahead_s, AI_BOOST_GAP_S) && attack_here)
        || within(gap_behind_s, AI_DEFEND_BOOST_GAP_S)
    {
        return (ErsMode::Balanced, true);
    }
    let alone = !within(gap_ahead_s, AI_ALONE_GAP_S) && !within(gap_behind_s, AI_ALONE_GAP_S);
    if alone && charge < AI_HARVEST_BELOW {
        return (ErsMode::Harvest, false);
    }
    (ErsMode::Balanced, false)
}

/// Off the throttle and the brake, the motor recovers this share of its
/// regen power against the crank.
pub const COAST_SHARE: f32 = 0.3;
/// In Harvest mode on the throttle, the motor recovers this share of its
/// regen power against the crank.
pub const HARVEST_SHARE: f32 = 0.5;
/// Balanced mode's pacing: the motor gives the share of the budget left
/// over the share of the lap left (so an even spend is full power and a
/// spend ahead of it is eased back smoothly), each with this added so the
/// end of the lap is not a division by nothing. A step (full power until
/// an allowance ran out, then none) cut every F1's motor two seconds after
/// the start line, and the field ran into each other at the first corner.
const PACE_LEAD: f32 = 0.05;
/// Balanced mode deploys fully above this charge and nothing below
/// [`SOC_FLOOR`], shares of the battery.
const SOC_EASE: f32 = 0.35;
const SOC_FLOOR: f32 = 0.15;
/// Balanced mode's throttle: nothing below the first, all of it from the
/// second.
const BALANCED_THROTTLE: (f32, f32) = (0.6, 0.95);
/// Heat recovery works from this throttle up.
const HEAT_THROTTLE: f32 = 0.8;

const KJ_PER_KWH: f32 = 3600.0;

/// `ers_flags`: bits 0-1 the mode, then what the motor is doing.
pub const ERS_FLAG_DEPLOYING: u8 = 4;
pub const ERS_FLAG_HARVESTING: u8 = 8;
pub const ERS_FLAG_BOOST: u8 = 16;

/// Share of the driven axle's braking power the motor may recover: the
/// rest is the hydraulics' and the tyres' own losses.
pub const REGEN_AXLE_SHARE: f32 = 0.9;

/// What the hybrid did this tick.
#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct MotorOutput {
    /// The motor's torque at the crank, Nm; negative while it recovers
    /// against the engine. Nothing for a front-axle motor.
    pub crank_torque_nm: f32,
    /// A front-axle motor's torque at the front axle, Nm (`HybridAxle::Front`):
    /// negative while it recovers coasting or harvesting (under braking it
    /// recovers through `regen_w`). Nothing for a crank motor.
    pub front_axle_torque_nm: f32,
    /// The power it is recovering from the driven axle under braking, W:
    /// what brake-by-wire takes off that axle's hydraulic brakes.
    pub regen_w: f32,
}

/// A full battery and a fresh lap budget: a car sent out of the garage or
/// lined up on a grid. A fresh stint too.
pub fn charge_full(state: &mut CarState, config: &CarConfig) {
    if config.hybrid.enabled {
        state.hybrid_battery_kwh = config.hybrid.battery_capacity_kwh;
    }
    state.ers_deployed_kj = 0.0;
    state.ers_override_kj = 0.0;
    state.ers_lap_mark = state.current_lap;
    new_stint(state, config);
}

/// A new stint's budget: a pit stop. The whole car's stint energy
/// (`[fuel] stint_energy_mj`) starts again with it.
pub fn new_stint(state: &mut CarState, config: &CarConfig) {
    state.ers_stint_kj = 0.0;
    state.stint_energy_used_mj = 0.0;
    publish(state, config);
}

/// Energy the motor may still deploy this stint, kJ: its own stint
/// budget, and the whole car's (`[fuel] stint_energy_mj`).
fn stint_left_kj(state: &CarState, config: &CarConfig) -> f32 {
    config
        .hybrid
        .stint_kj
        .map(|s| s - state.ers_stint_kj)
        .unwrap_or(f32::INFINITY)
        .min(config.fuel.stint_left_mj(state.stint_energy_used_mj) * 1000.0)
}

/// Energy the overtake button may still spend this lap, kJ: its own
/// allowance where the car has one, else the lap budget's.
fn override_left_kj(state: &CarState, config: &CarConfig) -> f32 {
    match config.hybrid.override_kj_per_lap {
        Some(allowance) => allowance - state.ers_override_kj,
        None => config
            .hybrid
            .deploy_kj_per_lap
            .map(|b| b - state.ers_deployed_kj)
            .unwrap_or(f32::INFINITY),
    }
}

/// The deployment the motor may give this tick, 0..1 of its torque, from
/// the mode, the throttle, the speed and the energy left. `boost` is the
/// overtake button with an allowance of its own left.
fn deploy_share(
    state: &CarState,
    config: &CarConfig,
    mode: ErsMode,
    boost: bool,
    throttle: f32,
) -> f32 {
    let hybrid = &config.hybrid;
    if state.hybrid_battery_kwh <= 0.0 || state.speed_mps * 3.6 < hybrid.deploy_min_speed_kph {
        return 0.0;
    }
    if stint_left_kj(state, config) <= 0.0 {
        return 0.0;
    }
    if boost {
        return if override_left_kj(state, config) > 0.0 {
            throttle.clamp(0.0, 1.0)
        } else {
            0.0
        };
    }
    let budget_left = hybrid
        .deploy_kj_per_lap
        .map(|b| b - state.ers_deployed_kj)
        .unwrap_or(f32::INFINITY);
    if budget_left <= 0.0 {
        return 0.0;
    }
    match mode {
        ErsMode::Harvest => 0.0,
        ErsMode::Attack => throttle.clamp(0.0, 1.0),
        ErsMode::Balanced => {
            let (lo, hi) = BALANCED_THROTTLE;
            let pedal = ((throttle - lo) / (hi - lo)).clamp(0.0, 1.0);
            let capacity = hybrid.battery_capacity_kwh.max(1e-6);
            let soc = state.hybrid_battery_kwh / capacity;
            let charge = ((soc - SOC_FLOOR) / (SOC_EASE - SOC_FLOOR)).clamp(0.0, 1.0);
            let pace = match hybrid.deploy_kj_per_lap {
                Some(budget) if budget > 0.0 => balanced_pace(
                    state.ers_deployed_kj / budget,
                    state.ers_lap_share,
                    hybrid.deploy_early,
                ),
                _ => 1.0,
            };
            pedal * charge * pace
        }
    }
}

/// Balanced mode's pacing, 0..1 of the motor: the share of the lap budget
/// left (`spent_share` gone) over the share of the lap left at
/// `lap_share`, bent by the deploy map. The racing line paces its plan by
/// it too (`racing_line::build_laden`).
pub fn balanced_pace(spent_share: f32, lap_share: f32, deploy_early: f32) -> f32 {
    let budget_left = (1.0 - spent_share).max(0.0);
    let lap_left = (1.0 - spend_target(lap_share, deploy_early)).max(0.0);
    ((budget_left + PACE_LEAD) / (lap_left + PACE_LEAD)).clamp(0.0, 1.0)
}

/// The share of the lap budget Balanced mode means to have spent at
/// `lap_share` round the lap: the lap share itself for an even map, bent
/// earlier (`deploy_early` > 0) or later.
fn spend_target(lap_share: f32, deploy_early: f32) -> f32 {
    if deploy_early == 0.0 {
        lap_share
    } else {
        lap_share.max(0.0).powf((1.0 - deploy_early).max(0.2))
    }
}

/// One tick of the hybrid: the motor's torque at the crank (Nm, negative
/// while it recovers against the engine) and the power it recovers from
/// the driven axle's brakes, moving the battery and the lap's and the
/// stint's spend. `lap_share` is the car's progress round the lap, 0..1,
/// for the pacing.
pub fn update(
    state: &mut CarState,
    config: &CarConfig,
    input: &PlayerInputData,
    engine_rpm: f32,
    lap_share: f32,
    dt: f32,
) -> MotorOutput {
    let out = step(state, config, input, engine_rpm, lap_share, dt);
    publish(state, config);
    out
}

fn step(
    state: &mut CarState,
    config: &CarConfig,
    input: &PlayerInputData,
    engine_rpm: f32,
    lap_share: f32,
    dt: f32,
) -> MotorOutput {
    state.ers_deploying = false;
    state.ers_harvesting = false;
    if let Some(mode) = input.ers_mode {
        state.ers_mode = ErsMode::from_u8(mode) as u8;
    }
    state.ers_boost = input.ers_boost;
    let hybrid = &config.hybrid;
    if !hybrid.enabled {
        return MotorOutput::default();
    }

    // Seed the battery from config on first use (serde default is -1.0).
    if state.hybrid_battery_kwh < 0.0 {
        state.hybrid_battery_kwh = hybrid.battery_capacity_kwh;
    }
    // A new lap, a new budget: on crossing the line, which is not always
    // when the lap counter moves (pole's lap 1 starts on green, behind the
    // line, and a budget reset there was spent again by the line).
    let share = lap_share.clamp(0.0, 1.0);
    let crossed = state.ers_lap_share > 0.75 && share < 0.25;
    if crossed || state.current_lap != state.ers_lap_mark {
        state.ers_lap_mark = state.current_lap;
        state.ers_deployed_kj = 0.0;
        state.ers_override_kj = 0.0;
    }
    state.ers_lap_share = share;

    let capacity = hybrid.battery_capacity_kwh;
    let front = hybrid.axle == HybridAxle::Front;
    // The motor's speed: the crank's, or the front wheels'.
    let omega = if front {
        state.speed_mps / config.wheel_radius_m.max(0.05)
    } else {
        (engine_rpm / 60.0) * 2.0 * PI
    }; // rad/s
       // What the motor can give at that speed: its curve, or its peak.
    let torque_cap = hybrid.torque_at(omega * 60.0 / (2.0 * PI));
    let charge = |state: &mut CarState, kw: f32| {
        state.hybrid_battery_kwh = (state.hybrid_battery_kwh + kw * dt / KJ_PER_KWH).min(capacity);
    };

    // Heat recovery rides on the exhaust: it costs the engine nothing.
    if hybrid.heat_recovery_kw > 0.0 && input.throttle >= HEAT_THROTTLE {
        charge(state, hybrid.heat_recovery_kw * input.throttle);
    }

    let regen_kw = hybrid
        .regen_max_power_kw
        .min(hybrid.battery_max_charge_kw)
        .max(0.0);
    let room = state.hybrid_battery_kwh < capacity;
    let rolling = state.speed_mps > 3.0 && (front || state.gear > 0);

    // Under braking: what the driven axle's tyres are taking out of the car
    // charges the battery, up to the regen power. Not free: with brake-by-
    // wire it is energy the discs would have taken (`physics` takes it off
    // the hydraulics), and without it is braking on top of the pedal.
    if input.brake > 0.1 && state.speed_mps > 3.0 {
        if room && regen_kw > 0.0 {
            let axle_kw = state.driven_axle_brake_w.max(0.0) / 1000.0 * REGEN_AXLE_SHARE;
            let kw = (regen_kw * input.brake).min(axle_kw);
            if kw > 0.0 {
                charge(state, kw);
                state.ers_harvesting = true;
                return MotorOutput {
                    regen_w: kw * 1000.0,
                    ..Default::default()
                };
            }
        }
        return MotorOutput::default();
    }

    let mode = ErsMode::from_u8(state.ers_mode);
    // The overtake button: Attack on its own allowance where the car has
    // one (nothing more once it is spent), on the lap budget otherwise.
    let boost = state.ers_boost && override_left_kj(state, config) > 0.0;

    // Recovery against the crank: off the pedals, or Harvest on the
    // throttle.
    let recover_share = if input.throttle < 0.05 {
        COAST_SHARE
    } else if mode == ErsMode::Harvest {
        HARVEST_SHARE
    } else {
        0.0
    };
    if recover_share > 0.0 {
        if !(room && rolling && regen_kw > 0.0 && omega > 1.0) {
            return MotorOutput::default();
        }
        let kw = regen_kw * recover_share;
        let torque = (kw * 1000.0 / omega).min(torque_cap);
        charge(state, torque * omega / 1000.0);
        state.ers_harvesting = true;
        return MotorOutput::at(hybrid.axle, -torque);
    }

    let share = deploy_share(state, config, mode, boost, input.throttle);
    if share <= 0.0 {
        return MotorOutput::default();
    }
    let power_limit_w = hybrid
        .motor_max_power_kw
        .min(hybrid.battery_max_discharge_kw)
        .max(0.0)
        * 1000.0;
    let torque_from_power = if omega > 1.0 {
        power_limit_w / omega
    } else {
        torque_cap
    };
    let mut torque = (share * torque_cap).min(torque_from_power).max(0.0);
    // Never past the budget, the allowance, the stint or the charge within
    // the tick.
    let mut drawn_kj = torque * omega.max(1.0) * dt / 1000.0;
    let on_override = boost && hybrid.override_kj_per_lap.is_some();
    let budget_left = if on_override {
        override_left_kj(state, config)
    } else {
        hybrid
            .deploy_kj_per_lap
            .map(|b| b - state.ers_deployed_kj)
            .unwrap_or(f32::INFINITY)
    };
    let left_kj = budget_left
        .min(stint_left_kj(state, config))
        .min(state.hybrid_battery_kwh * KJ_PER_KWH);
    if drawn_kj > left_kj {
        torque *= (left_kj / drawn_kj).max(0.0);
        drawn_kj = left_kj.max(0.0);
    }
    state.hybrid_battery_kwh = (state.hybrid_battery_kwh - drawn_kj / KJ_PER_KWH).max(0.0);
    if on_override {
        state.ers_override_kj += drawn_kj;
    } else {
        state.ers_deployed_kj += drawn_kj;
    }
    state.ers_stint_kj += drawn_kj;
    if config.fuel.stint_energy_mj.is_some() {
        state.stint_energy_used_mj += drawn_kj / 1000.0;
    }
    state.ers_deploying = torque > 0.0;
    MotorOutput::at(hybrid.axle, torque)
}

impl MotorOutput {
    /// The motor's `torque_nm` where it drives: the crank or the front axle.
    fn at(axle: HybridAxle, torque_nm: f32) -> Self {
        match axle {
            HybridAxle::Crank => MotorOutput {
                crank_torque_nm: torque_nm,
                ..Default::default()
            },
            HybridAxle::Front => MotorOutput {
                front_axle_torque_nm: torque_nm,
                ..Default::default()
            },
        }
    }
}

/// What brake-by-wire takes off each axle's hydraulics (front, rear), N,
/// for `regen_axle_n`, the recovery's braking force on each axle this
/// tick: the recovery itself, or on a car with `bbw_response_s` an
/// estimate that follows it with that time constant, so the balance
/// migrates while the hydraulics catch up (rearward as the recovery comes
/// in, forward as it drops away with the battery full).
pub fn bbw_offset_n(
    state: &mut CarState,
    config: &CarConfig,
    regen_axle_n: [f32; 2],
    dt: f32,
) -> [f32; 2] {
    let tau = config.hybrid.bbw_response_s;
    if tau <= 0.0 {
        state.bbw_offset_n = regen_axle_n;
    } else {
        let k = 1.0 - (-dt / tau).exp();
        for (offset, regen) in state.bbw_offset_n.iter_mut().zip(regen_axle_n) {
            *offset += (regen - *offset) * k;
        }
    }
    state.bbw_offset_n
}

/// The motor as the racing line plans it (`racing_line`): what it gives at
/// a speed, as [`update`] would give it there. A crank motor turns with
/// the engine, in the gear the automatic box holds at that speed (it
/// upshifts at `physics::auto_upshift_rpm`); a front-axle motor turns
/// with the front wheels.
#[derive(Debug, Clone)]
pub struct MotorPlan {
    hybrid: crate::data::HybridConfig,
    min_mps: f32,
    power_limit_w: f32,
    wheel_radius_m: f32,
    idle_rpm: f32,
    /// Per forward gear, its overall ratio and the crank rpm it is held
    /// to (infinite for top gear).
    gears: Vec<(f32, f32)>,
}

impl MotorPlan {
    /// `None` for a car without a hybrid.
    pub fn new(config: &CarConfig) -> Option<Self> {
        let hybrid = &config.hybrid;
        if !hybrid.enabled {
            return None;
        }
        let gears = (1..config.gear_ratios.len())
            .filter_map(|g| {
                let ratio = config.gear_ratios[g].abs() * config.final_drive_ratio;
                let upshift =
                    crate::physics::auto_upshift_rpm(config, g as i8).unwrap_or(f32::INFINITY);
                (ratio > 0.0).then_some((ratio, upshift))
            })
            .collect();
        Some(Self {
            hybrid: hybrid.clone(),
            min_mps: hybrid.deploy_min_speed_kph / 3.6,
            power_limit_w: hybrid
                .motor_max_power_kw
                .min(hybrid.battery_max_discharge_kw)
                .max(0.0)
                * 1000.0,
            wheel_radius_m: config.wheel_radius_m.max(0.05),
            idle_rpm: config.idle_rpm,
            gears,
        })
    }

    /// The motor's speed at `speed_mps`, rad/s.
    fn omega(&self, speed_mps: f32) -> f32 {
        let wheel = speed_mps.max(0.0) / self.wheel_radius_m;
        if self.hybrid.axle == HybridAxle::Front {
            return wheel;
        }
        let rpm_per_ratio = wheel * 60.0 / (2.0 * PI);
        let rpm = self
            .gears
            .iter()
            .map(|&(ratio, upshift)| (ratio * rpm_per_ratio, upshift))
            .find(|&(rpm, upshift)| rpm <= upshift)
            .or_else(|| self.gears.last().map(|&(r, _)| (r * rpm_per_ratio, 0.0)))
            .map_or(0.0, |(rpm, _)| rpm);
        rpm.max(self.idle_rpm) * 2.0 * PI / 60.0
    }

    /// What the motor gives at `speed_mps`, W, deploying `share` of its
    /// torque: none below the minimum deployment speed, else the torque
    /// its curve has at the rpm it turns at (its peak without one), capped
    /// by its power and the battery's.
    pub fn power_w(&self, speed_mps: f32, share: f32) -> f32 {
        if speed_mps < self.min_mps || share <= 0.0 {
            return 0.0;
        }
        let omega = self.omega(speed_mps);
        let torque = share * self.hybrid.torque_at(omega * 60.0 / (2.0 * PI));
        (torque * omega).clamp(0.0, self.power_limit_w)
    }
}

/// Telemetry's three hybrid bytes: the charge in percent, the lap budget
/// left in percent (255: no budget, or no hybrid), and `ers_flags`.
pub fn telemetry_bytes(state: &CarState) -> (u8, u8, u8) {
    if state.ers_charge_pct == 255 {
        return (255, 255, 0);
    }
    let mut flags = state.ers_mode & 3;
    if state.ers_deploying {
        flags |= ERS_FLAG_DEPLOYING;
    }
    if state.ers_harvesting {
        flags |= ERS_FLAG_HARVESTING;
    }
    if state.ers_boost {
        flags |= ERS_FLAG_BOOST;
    }
    (state.ers_charge_pct, state.ers_budget_pct, flags)
}

/// Keep telemetry's percentages on the car.
fn publish(state: &mut CarState, config: &CarConfig) {
    let hybrid = &config.hybrid;
    if !hybrid.enabled {
        state.ers_charge_pct = 255;
        state.ers_budget_pct = 255;
        state.ers_stint_pct = 255;
        return;
    }
    state.ers_stint_pct = match hybrid.stint_kj {
        Some(s) if s > 0.0 => ((1.0 - state.ers_stint_kj / s) * 100.0)
            .round()
            .clamp(0.0, 100.0) as u8,
        _ => 255,
    };
    state.ers_charge_pct = if hybrid.battery_capacity_kwh > 0.0 {
        (state.hybrid_battery_kwh.max(0.0) / hybrid.battery_capacity_kwh * 100.0)
            .round()
            .clamp(0.0, 100.0) as u8
    } else {
        0
    };
    state.ers_budget_pct = match hybrid.deploy_kj_per_lap {
        Some(b) if b > 0.0 => ((1.0 - state.ers_deployed_kj / b) * 100.0)
            .round()
            .clamp(0.0, 100.0) as u8,
        _ => 255,
    };
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::data::HybridConfig;

    #[test]
    fn the_ai_boosts_to_attack_and_defend_and_harvests_alone() {
        use ErsMode::*;
        assert_eq!(
            ai_ers(0.8, Some(0.5), None, None),
            (Balanced, true),
            "attack"
        );
        assert_eq!(
            ai_ers(0.8, None, Some(0.3), None),
            (Balanced, true),
            "defend"
        );
        assert_eq!(ai_ers(0.8, Some(1.5), Some(1.0), None), (Balanced, false));
        assert_eq!(
            ai_ers(0.2, None, None, None),
            (Harvest, false),
            "low, alone"
        );
        assert_eq!(
            ai_ers(0.2, Some(1.5), None, None),
            (Balanced, false),
            "low, but racing a car"
        );
        assert_eq!(ai_ers(0.2, Some(3.0), Some(5.0), None), (Harvest, false));
        // The attack is saved for a long straight; a defence is anywhere.
        assert_eq!(ai_ers(0.8, Some(0.5), None, Some(0.3)), (Balanced, false));
        assert_eq!(ai_ers(0.8, Some(0.5), None, Some(1.0)), (Balanced, true));
        assert_eq!(ai_ers(0.8, None, Some(0.3), Some(0.1)), (Balanced, true));
    }

    /// A lap of two straights, 300 m and 100 m, each after a corner: a
    /// point knows its straight, a corner the one after it.
    #[test]
    fn the_straights_are_measured_and_a_corner_knows_the_next() {
        use crate::racing_line::{LinePhase, RacingLineProfile};
        let mut phases = vec![LinePhase::Brake; 20];
        phases.extend(vec![LinePhase::Throttle; 60]);
        phases.extend(vec![LinePhase::Partial; 20]);
        phases.extend(vec![LinePhase::Throttle; 20]);
        // Rotated so the loop's seam falls inside the short straight.
        phases.rotate_left(110);
        let n = phases.len();
        let profile = RacingLineProfile {
            spacing_m: 5.0,
            points: vec![[0.0; 3]; n],
            speed_mps: vec![50.0; n],
            phases,
            unpaced_mps: vec![50.0; n],
        };
        let s = straights(&profile);
        assert_eq!(s.longest_m, 300.0);
        // The short straight across the seam is measured whole.
        assert_eq!(s.share_at(0), Some(100.0 / 300.0));
        assert_eq!(s.share_at(n - 1), Some(100.0 / 300.0));
        // The first corner leads onto the long straight.
        assert_eq!(s.share_at(10), Some(1.0));
        assert_eq!(s.share_at(40), Some(1.0));
        // The partial corner onto the short one.
        assert_eq!(s.share_at(95), Some(100.0 / 300.0));
        assert_eq!(s.share_at(n), None);
    }

    fn car(budget: Option<f32>) -> CarConfig {
        CarConfig {
            hybrid: HybridConfig {
                enabled: true,
                battery_capacity_kwh: 1.1,
                battery_max_discharge_kw: 300.0,
                battery_max_charge_kw: 250.0,
                motor_max_torque_nm: 300.0,
                motor_max_power_kw: 300.0,
                regen_max_power_kw: 250.0,
                deploy_kj_per_lap: budget,
                deploy_min_speed_kph: 0.0,
                heat_recovery_kw: 0.0,
                ..HybridConfig::default()
            },
            ..CarConfig::default()
        }
    }

    /// The motor's crank torque this tick.
    fn torque(
        state: &mut CarState,
        config: &CarConfig,
        input: &PlayerInputData,
        rpm: f32,
        share: f32,
    ) -> f32 {
        update(state, config, input, rpm, share, DT).crank_torque_nm
    }

    fn rolling(config: &CarConfig) -> CarState {
        let slot = crate::data::GridSlot {
            position: 1,
            x: 0.0,
            y: 0.0,
            z: 0.0,
            yaw_rad: 0.0,
        };
        let mut state = CarState::new(uuid::Uuid::nil(), config.id, &slot);
        state.speed_mps = 60.0;
        state.gear = 6;
        charge_full(&mut state, config);
        state
    }

    fn input(throttle: f32, brake: f32, mode: ErsMode, boost: bool) -> PlayerInputData {
        PlayerInputData {
            throttle,
            brake,
            ers_mode: Some(mode as u8),
            ers_boost: boost,
            ..Default::default()
        }
    }

    const DT: f32 = 1.0 / 240.0;

    /// A motor that files a torque curve gives the curve's torque at its
    /// revs (capped at its peak); without one it gives its peak up to the
    /// power limit, as before.
    #[test]
    fn a_motor_with_a_curve_follows_it() {
        let flat = car(None);
        let mut curved = car(None);
        curved.hybrid.torque_curve = vec![
            crate::data::TorqueCurvePoint {
                rpm: 2000.0,
                torque_nm: 400.0,
            },
            crate::data::TorqueCurvePoint {
                rpm: 5000.0,
                torque_nm: 300.0,
            },
            crate::data::TorqueCurvePoint {
                rpm: 9000.0,
                torque_nm: 100.0,
            },
        ];
        let full = input(1.0, 0.0, ErsMode::Attack, false);
        let at =
            |config: &CarConfig, rpm: f32| torque(&mut rolling(config), config, &full, rpm, 0.5);
        // 300 kW at 9000 rpm is 318 Nm: the peak's 300 Nm without a curve.
        assert!((at(&flat, 9000.0) - 300.0).abs() < 0.5);
        assert!(
            (at(&curved, 9000.0) - 100.0).abs() < 0.5,
            "{}",
            at(&curved, 9000.0)
        );
        // Halfway down the curve, and the curve's 400 Nm held to the peak.
        assert!((at(&curved, 7000.0) - 200.0).abs() < 0.5);
        assert!((at(&curved, 2000.0) - 300.0).abs() < 0.5);
    }

    /// The racing line counts the motor as the tick gives it, at the revs
    /// it turns at in the gear the automatic box holds: a curve that falls
    /// with the revs gives less where the gear is high in its range, a
    /// front motor turns with the wheels, and below the minimum speed it
    /// gives nothing.
    #[test]
    fn the_plan_counts_the_motor_at_the_revs_it_turns_at() {
        let flat = car(None);
        let mut curved = flat.clone();
        curved.hybrid.torque_curve = vec![
            crate::data::TorqueCurvePoint {
                rpm: 2000.0,
                torque_nm: 400.0,
            },
            crate::data::TorqueCurvePoint {
                rpm: 9000.0,
                torque_nm: 100.0,
            },
        ];
        let (f, c) = (
            MotorPlan::new(&flat).unwrap(),
            MotorPlan::new(&curved).unwrap(),
        );
        let full = input(1.0, 0.0, ErsMode::Attack, false);
        let mut cost: f32 = 1.0;
        for v in [15.0, 30.0, 45.0, 60.0, 75.0] {
            let omega = c.omega(v);
            let rpm = omega * 60.0 / (2.0 * PI);
            assert!(
                rpm >= curved.idle_rpm && rpm <= curved.redline_rpm,
                "{rpm} rpm at {v} m/s"
            );
            let ticked = torque(&mut rolling(&curved), &curved, &full, rpm, 0.5) * omega;
            assert!(
                (c.power_w(v, 1.0) - ticked).abs() < 50.0,
                "{} W planned, {ticked} W ticked",
                c.power_w(v, 1.0)
            );
            assert!(c.power_w(v, 1.0) <= f.power_w(v, 1.0) + 1.0);
            assert!(c.power_w(v, 0.5) <= c.power_w(v, 1.0));
            cost = cost.min(c.power_w(v, 1.0) / f.power_w(v, 1.0));
        }
        assert!(cost < 0.8, "the falling curve costs the plan {cost}");

        let mut wec = flat.clone();
        wec.hybrid.deploy_min_speed_kph = 190.0;
        let w = MotorPlan::new(&wec).unwrap();
        assert_eq!(w.power_w(52.0, 1.0), 0.0);
        assert!(w.power_w(53.0, 1.0) > 0.0);

        let mut front = flat.clone();
        front.hybrid.axle = HybridAxle::Front;
        let fr = MotorPlan::new(&front).unwrap();
        assert!((fr.omega(30.0) - 30.0 / front.wheel_radius_m).abs() < 1e-3);
        assert!(MotorPlan::new(&CarConfig::default()).is_none());
    }

    /// A front-axle motor puts its torque on the front axle, turns with
    /// the front wheels (so its power limit is at their speed, whatever
    /// the gear or the revs), and coasts against the fronts in neutral too;
    /// a crank motor's figures are where they were.
    #[test]
    fn a_front_motor_drives_and_recovers_on_the_front_axle() {
        let crank = car(None);
        let mut front = car(None);
        front.hybrid.axle = HybridAxle::Front;
        front.wheel_radius_m = 0.35;
        // At the axle: the gearbox no longer multiplies it.
        front.hybrid.motor_max_torque_nm = 2000.0;
        let full = input(1.0, 0.0, ErsMode::Attack, false);
        let mut s = rolling(&crank);
        let out = update(&mut s, &crank, &full, 9000.0, 0.5, DT);
        assert!(out.crank_torque_nm > 0.0);
        assert_eq!(out.front_axle_torque_nm, 0.0);

        let mut s = rolling(&front);
        let out = update(&mut s, &front, &full, 9000.0, 0.5, DT);
        assert_eq!(out.crank_torque_nm, 0.0);
        // 300 kW at 60 m/s on a 0.35 m wheel: 1750 Nm at the axle, the
        // crank's 9000 rpm nothing to do with it.
        assert!((out.front_axle_torque_nm - 1750.0).abs() < 0.5);
        assert!(s.ers_deploying);
        // Faster, less torque for the same power: P / wheel speed.
        s.speed_mps = 90.0;
        let out = update(&mut s, &front, &full, 2000.0, 0.5, DT);
        let at_power = 300_000.0 / (90.0 / 0.35);
        assert!(
            (out.front_axle_torque_nm - at_power).abs() < 0.5,
            "{} vs {at_power}",
            out.front_axle_torque_nm
        );
        // Off the pedals in neutral it still coasts against the fronts.
        s.gear = 0;
        let coast = input(0.0, 0.0, ErsMode::Balanced, false);
        let out = update(&mut s, &front, &coast, 1000.0, 0.5, DT);
        assert!(out.front_axle_torque_nm < 0.0 && s.ers_harvesting);
        assert_eq!(out.crank_torque_nm, 0.0);
    }

    #[test]
    fn modes_deploy_differently_at_part_and_full_throttle() {
        let config = car(None);
        let run = |throttle, mode| {
            let mut s = rolling(&config);
            s.hybrid_battery_kwh = 0.8;
            torque(
                &mut s,
                &config,
                &input(throttle, 0.0, mode, false),
                10000.0,
                0.5,
            )
        };
        assert!(run(1.0, ErsMode::Harvest) < 0.0, "harvest never deploys");
        assert!(run(1.0, ErsMode::Balanced) > 250.0);
        assert_eq!(
            run(0.5, ErsMode::Balanced),
            0.0,
            "balanced waits for full throttle"
        );
        assert!(run(0.5, ErsMode::Attack) > 100.0);
        assert!(
            run(0.5, ErsMode::Harvest) < 0.0,
            "harvest charges against the crank"
        );
    }

    #[test]
    fn the_deploy_map_spends_the_budget_earlier_or_later_in_the_lap() {
        // A fifth of the way round with three tenths of the budget gone:
        // ahead of an even spend, so Balanced eases back; an early map is
        // still behind its curve, a late one further ahead of it.
        let run = |deploy_early: f32| {
            let mut config = car(Some(2000.0));
            config.hybrid.deploy_early = deploy_early;
            let mut s = rolling(&config);
            s.hybrid_battery_kwh = 0.8;
            s.ers_deployed_kj = 600.0;
            torque(
                &mut s,
                &config,
                &input(1.0, 0.0, ErsMode::Balanced, false),
                10000.0,
                0.2,
            )
        };
        let (early, even, late) = (run(0.5), run(0.0), run(-0.5));
        assert!(early > even && even > late, "{early} {even} {late}");
        assert_eq!(spend_target(0.4, 0.0), 0.4);
    }

    #[test]
    fn the_lap_budget_runs_out_and_comes_back_at_the_line() {
        let config = car(Some(2000.0));
        let mut s = rolling(&config);
        let flat = input(1.0, 0.0, ErsMode::Attack, false);
        let mut ticks = 0;
        while torque(&mut s, &config, &flat, 10000.0, 0.3) > 0.0 {
            ticks += 1;
            assert!(ticks < 240 * 60);
        }
        // 2 MJ at 300 kW: under seven seconds.
        let seconds = ticks as f32 * DT;
        assert!((6.0..7.0).contains(&seconds), "{seconds}");
        assert!(s.ers_deployed_kj <= 2000.0 + 1e-3);
        s.current_lap += 1;
        assert!(torque(&mut s, &config, &flat, 10000.0, 0.01) > 0.0);

        // Crossing the line resets it too, whatever the lap counter says.
        let mut pole = rolling(&config);
        pole.ers_deployed_kj = 2000.0;
        torque(&mut pole, &config, &flat, 10000.0, 0.99);
        assert!(pole.ers_deployed_kj >= 2000.0);
        torque(&mut pole, &config, &flat, 10000.0, 0.001);
        assert!(pole.ers_deployed_kj < 10.0, "{}", pole.ers_deployed_kj);
    }

    #[test]
    fn a_stint_budget_runs_out_and_a_stop_renews_it() {
        let mut config = car(None);
        config.hybrid.stint_kj = Some(1000.0);
        let mut s = rolling(&config);
        let flat = input(1.0, 0.0, ErsMode::Attack, false);
        let mut ticks = 0;
        while torque(&mut s, &config, &flat, 10000.0, 0.3) > 0.0 {
            ticks += 1;
            assert!(ticks < 240 * 60);
        }
        assert!((s.ers_stint_kj - 1000.0).abs() < 1.0, "{}", s.ers_stint_kj);
        assert_eq!(s.ers_stint_pct, 0);
        // A new lap does not help; a stop does.
        s.current_lap += 1;
        assert_eq!(torque(&mut s, &config, &flat, 10000.0, 0.01), 0.0);
        new_stint(&mut s, &config);
        assert_eq!(s.ers_stint_pct, 100);
        assert!(torque(&mut s, &config, &flat, 10000.0, 0.02) > 0.0);
        // A car without the rule reports none.
        let plain = car(None);
        let mut p = rolling(&plain);
        torque(&mut p, &plain, &flat, 10000.0, 0.1);
        assert_eq!(p.ers_stint_pct, 255);
    }

    #[test]
    fn the_overtake_button_spends_its_own_allowance_and_then_nothing() {
        let mut config = car(Some(4000.0));
        config.hybrid.override_kj_per_lap = Some(300.0);
        let mut s = rolling(&config);
        // Balanced, paced: a quarter of the way round with half the budget
        // gone is eased back ...
        s.ers_deployed_kj = 2000.0;
        let paced = torque(
            &mut s,
            &config,
            &input(1.0, 0.0, ErsMode::Balanced, false),
            10000.0,
            0.25,
        );
        // ... and the button is full power, from its own allowance: the
        // lap's spend does not move.
        let before = s.ers_deployed_kj;
        let boost = input(1.0, 0.0, ErsMode::Balanced, true);
        let full = torque(&mut s, &config, &boost, 10000.0, 0.25);
        assert!(full > paced * 1.2, "{full} against {paced}");
        assert!((s.ers_deployed_kj - before).abs() < 1e-3);
        assert!(s.ers_override_kj > 0.0);
        // Hold it until the allowance is gone: then it does nothing more.
        let mut ticks = 0;
        while torque(&mut s, &config, &boost, 10000.0, 0.25) > paced * 1.05 {
            ticks += 1;
            assert!(ticks < 240 * 30);
        }
        assert!(
            (s.ers_override_kj - 300.0).abs() < 1.0,
            "{}",
            s.ers_override_kj
        );
        assert!(s.ers_boost, "the button is still held, for the HUD");
        // The next lap gives it back.
        s.current_lap += 1;
        s.ers_deployed_kj = 0.0;
        assert!(torque(&mut s, &config, &boost, 10000.0, 0.01) > 0.0);
    }

    #[test]
    fn braking_recovers_only_what_the_driven_axle_takes_out() {
        let config = car(None);
        let brake = input(0.0, 1.0, ErsMode::Balanced, false);
        // Nothing yet known of the axle's braking: nothing recovered.
        let mut s = rolling(&config);
        s.hybrid_battery_kwh = 0.5;
        s.driven_axle_brake_w = 0.0;
        let out = update(&mut s, &config, &brake, 10000.0, 0.5, DT);
        assert_eq!(out.regen_w, 0.0);
        assert!((s.hybrid_battery_kwh - 0.5).abs() < 1e-7);
        // The axle braking at 100 kW: 90 of it recovered.
        s.driven_axle_brake_w = 100_000.0;
        let out = update(&mut s, &config, &brake, 10000.0, 0.5, DT);
        assert!((out.regen_w - 90_000.0).abs() < 1.0, "{out:?}");
        assert!(s.hybrid_battery_kwh > 0.5 && s.ers_harvesting);
        // Never more than the regen power.
        s.driven_axle_brake_w = 1_000_000.0;
        let out = update(&mut s, &config, &brake, 10000.0, 0.5, DT);
        assert!((out.regen_w - 250_000.0).abs() < 1.0, "{out:?}");
    }

    #[test]
    fn balanced_paces_the_budget_over_the_lap() {
        let config = car(Some(4000.0));
        let flat = input(1.0, 0.0, ErsMode::Balanced, false);
        let at = |spent: f32, share: f32, input: &PlayerInputData| {
            let mut s = rolling(&config);
            s.ers_deployed_kj = spent;
            torque(&mut s, &config, input, 10000.0, share)
        };
        let full = at(0.0, 0.0, &flat);
        // Half the budget gone a quarter of the way round: eased back ...
        let ahead = at(2000.0, 0.25, &flat);
        assert!(
            ahead > 0.5 * full && ahead < 0.8 * full,
            "{ahead} of {full}"
        );
        // ... the overtake button is not ...
        let boost = input(1.0, 0.0, ErsMode::Balanced, true);
        assert!((at(2000.0, 0.25, &boost) - full).abs() < 1e-3);
        // ... and half of it gone half way round is full power again.
        assert!((at(2000.0, 0.5, &flat) - full).abs() < 1e-3);
    }

    #[test]
    fn braking_and_coasting_charge_and_a_hypercar_waits_for_its_speed() {
        let config = car(None);
        let mut s = rolling(&config);
        s.hybrid_battery_kwh = 0.5;
        s.driven_axle_brake_w = 200_000.0;
        torque(
            &mut s,
            &config,
            &input(0.0, 1.0, ErsMode::Balanced, false),
            10000.0,
            0.5,
        );
        assert!(s.hybrid_battery_kwh > 0.5 && s.ers_harvesting);
        let before = s.hybrid_battery_kwh;
        let coast = torque(
            &mut s,
            &config,
            &input(0.0, 0.0, ErsMode::Balanced, false),
            10000.0,
            0.5,
        );
        assert!(coast < 0.0 && s.hybrid_battery_kwh > before);

        let mut hyper = car(None);
        hyper.hybrid.deploy_min_speed_kph = 190.0;
        let mut slow = rolling(&hyper);
        slow.speed_mps = 45.0;
        let flat = input(1.0, 0.0, ErsMode::Attack, false);
        assert_eq!(torque(&mut slow, &hyper, &flat, 8000.0, 0.5), 0.0);
        slow.speed_mps = 55.0;
        assert!(torque(&mut slow, &hyper, &flat, 8000.0, 0.5) > 0.0);
    }

    #[test]
    fn balanced_keeps_a_floor_of_charge() {
        let config = car(None);
        let mut s = rolling(&config);
        s.hybrid_battery_kwh = 1.1 * 0.1;
        let flat = input(1.0, 0.0, ErsMode::Balanced, false);
        assert_eq!(torque(&mut s, &config, &flat, 10000.0, 0.5), 0.0);
        let attack = input(1.0, 0.0, ErsMode::Attack, false);
        assert!(torque(&mut s, &config, &attack, 10000.0, 0.5) > 0.0);
    }

    #[test]
    fn telemetry_reports_charge_budget_and_mode() {
        let config = car(Some(4000.0));
        let mut s = rolling(&config);
        update(
            &mut s,
            &config,
            &input(1.0, 0.0, ErsMode::Attack, true),
            10000.0,
            0.1,
            DT,
        );
        let (soc, lap, flags) = telemetry_bytes(&s);
        assert_eq!(soc, 100);
        assert_eq!(lap, 100);
        assert_eq!(flags & 3, ErsMode::Attack as u8);
        assert!(flags & ERS_FLAG_DEPLOYING != 0 && flags & ERS_FLAG_BOOST != 0);
        let plain = CarConfig::default();
        let mut p = rolling(&plain);
        update(
            &mut p,
            &plain,
            &input(1.0, 0.0, ErsMode::Attack, true),
            10000.0,
            0.1,
            DT,
        );
        assert_eq!(telemetry_bytes(&p), (255, 255, 0));
    }
}
