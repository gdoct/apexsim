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
//!   *Balanced* (the default, and the AI's) deploys at full throttle,
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
//! - a **manual override allowance** (`override_kj_per_lap`): what the
//!   overtake button may spend per lap over and above the paced lap
//!   budget; with it the button draws on its own allowance and leaves the
//!   budget alone, and once it is spent the button does nothing more that
//!   lap. Without it the button is Attack on the lap budget, as before.
//!
//! Recovery: under braking the motor charges with the energy the driven
//! axle's tyres are actually taking out of the car, up to the regen power
//! ([`MotorOutput::regen_w`]: last tick's braking power on that axle,
//! [`REGEN_AXLE_SHARE`] of it), and with **brake-by-wire** (`brake_by_wire`,
//! the default) that recovery stands in for the axle's hydraulic brakes,
//! so the pedal decelerates the car the same whether the battery takes
//! the energy or the discs do, and the discs run cooler for it; without
//! it the recovery brakes the driven axle on top of the pads. Off the
//! throttle and off the brake it charges at [`COAST_SHARE`] of the regen
//! power against the crank (a little more engine braking), and in Harvest
//! mode on the throttle at [`HARVEST_SHARE`] of it, again against the
//! crank.
//!
//! Telemetry carries the charge, the lap budget left and the mode and what
//! the motor is doing ([`telemetry_bytes`]).

use std::f32::consts::PI;

use crate::data::{CarConfig, CarState, PlayerInputData};

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

/// The mode a car starts in and the AI drives in.
pub const DEFAULT_MODE: u8 = ErsMode::Balanced as u8;

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
    /// against the engine.
    pub crank_torque_nm: f32,
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

/// A new stint's budget: a pit stop.
pub fn new_stint(state: &mut CarState, config: &CarConfig) {
    state.ers_stint_kj = 0.0;
    publish(state, config);
}

/// Energy the motor may still deploy this stint, kJ.
fn stint_left_kj(state: &CarState, config: &CarConfig) -> f32 {
    config
        .hybrid
        .stint_kj
        .map(|s| s - state.ers_stint_kj)
        .unwrap_or(f32::INFINITY)
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
                Some(budget) if budget > 0.0 => {
                    let budget_left = (1.0 - state.ers_deployed_kj / budget).max(0.0);
                    let lap_left = (1.0 - state.ers_lap_share).max(0.0);
                    ((budget_left + PACE_LEAD) / (lap_left + PACE_LEAD)).clamp(0.0, 1.0)
                }
                _ => 1.0,
            };
            pedal * charge * pace
        }
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
    let omega = (engine_rpm / 60.0) * 2.0 * PI; // rad/s
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
    let rolling = state.speed_mps > 3.0 && state.gear > 0;

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
                    crank_torque_nm: 0.0,
                    regen_w: kw * 1000.0,
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
        let torque = (kw * 1000.0 / omega).min(hybrid.motor_max_torque_nm);
        charge(state, torque * omega / 1000.0);
        state.ers_harvesting = true;
        return MotorOutput {
            crank_torque_nm: -torque,
            regen_w: 0.0,
        };
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
        hybrid.motor_max_torque_nm
    };
    let mut torque = (share * hybrid.motor_max_torque_nm)
        .min(torque_from_power)
        .max(0.0);
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
    state.ers_deploying = torque > 0.0;
    MotorOutput {
        crank_torque_nm: torque,
        regen_w: 0.0,
    }
}

/// The deployment the plan may count on at `speed_mps`: the motor's power,
/// or none below the car's minimum deployment speed.
pub fn plan_power_w(config: &CarConfig, speed_mps: f32) -> f32 {
    let hybrid = &config.hybrid;
    if !hybrid.enabled || speed_mps * 3.6 < hybrid.deploy_min_speed_kph {
        0.0
    } else {
        hybrid.motor_max_power_kw * 1000.0
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
