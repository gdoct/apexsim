//! The engine's coolant: heated by the power the engine makes, cooled by
//! the air through its radiator.
//!
//! One thermal mass (the coolant and the block), heated by
//! [`HEAT_PER_SHAFT_W`] of every watt of combustion power (the fuel's heat
//! that goes into the water, about as much as reaches the crank) plus an
//! idle trickle, and cooled by the radiator: the air through it grows with
//! the car's speed, and a car in the tow of another breathes the slower air
//! of its wake (`CarState::wake`). A fan keeps a car in the pits or on the
//! grid from boiling.
//!
//! Every car's radiator is sized from its own power
//! ([`radiator_conductance`]): at [`REFERENCE_POWER_SHARE`] of its peak at
//! [`REFERENCE_SPEED_MPS`] in air at [`REFERENCE_AIR_C`] it runs at
//! [`TARGET_C`], so a car on a mild day at racing pace is where its
//! engineers put it, and a hot day, a slow circuit, a long tow or a
//! `[engine] radiator_scale` under 1 run it hotter. Above [`OVERHEAT_C`] the
//! engine gives up power ([`power_factor`]).

/// Coolant heat per watt of combustion power at the crank.
pub const HEAT_PER_SHAFT_W: f32 = 0.9;
/// Heat an idling engine puts into its coolant, W.
const IDLE_HEAT_W: f32 = 6_000.0;
/// The coolant and block, J/K: a minute or so to settle.
const HEAT_CAPACITY: f32 = 120_000.0;
/// The fan's airflow, as m/s of road speed through the radiator.
const FAN_EQUIVALENT_MPS: f32 = 6.0;

/// Where a radiator is sized: the share of peak power, the speed, the air
/// and the coolant it holds there.
pub const REFERENCE_POWER_SHARE: f32 = 0.65;
pub const REFERENCE_SPEED_MPS: f32 = 60.0;
pub const REFERENCE_AIR_C: f32 = 25.0;
pub const TARGET_C: f32 = 95.0;

/// Above this the engine protects itself.
pub const OVERHEAT_C: f32 = 112.0;
/// Power lost per degree over it, and the least it keeps.
const OVERHEAT_LOSS_PER_C: f32 = 0.02;
const OVERHEAT_FLOOR: f32 = 0.6;

/// What the coolant is at when a car goes out: its crew warm it first.
pub const START_C: f32 = 80.0;

/// The radiator's conductance per m/s of air through it, W/K, for an engine
/// of `max_power_w` (times `radiator_scale`).
pub fn radiator_conductance(max_power_w: f32, radiator_scale: f32) -> f32 {
    let heat = HEAT_PER_SHAFT_W * REFERENCE_POWER_SHARE * max_power_w.max(1.0) + IDLE_HEAT_W;
    radiator_scale.max(0.1) * heat / ((TARGET_C - REFERENCE_AIR_C) * REFERENCE_SPEED_MPS)
}

/// The engine's power at a coolant temperature, as a share of its best.
pub fn power_factor(coolant_c: f32) -> f32 {
    if coolant_c <= OVERHEAT_C {
        return 1.0;
    }
    (1.0 - OVERHEAT_LOSS_PER_C * (coolant_c - OVERHEAT_C)).max(OVERHEAT_FLOOR)
}

/// Advance the coolant by `dt`: `shaft_w` of combustion power, air at
/// `airflow_mps` through a radiator of `conductance` W/K per m/s.
pub fn step(
    coolant_c: &mut f32,
    shaft_w: f32,
    airflow_mps: f32,
    air_c: f32,
    conductance: f32,
    dt: f32,
) {
    let heat = HEAT_PER_SHAFT_W * shaft_w.max(0.0) + IDLE_HEAT_W;
    let flow = airflow_mps.max(0.0).max(FAN_EQUIVALENT_MPS);
    let cooling = conductance * flow * (*coolant_c - air_c);
    *coolant_c += (heat - cooling) * dt / HEAT_CAPACITY;
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn at_the_reference_it_settles_on_the_target() {
        let power = 400_000.0;
        let k = radiator_conductance(power, 1.0);
        let mut t = START_C;
        for _ in 0..(240 * 60 * 10) {
            step(
                &mut t,
                REFERENCE_POWER_SHARE * power,
                REFERENCE_SPEED_MPS,
                REFERENCE_AIR_C,
                k,
                1.0 / 240.0,
            );
        }
        assert!((t - TARGET_C).abs() < 0.5, "{t}");
    }

    #[test]
    fn a_tow_on_a_hot_day_runs_it_hot_and_it_loses_power() {
        let power = 400_000.0;
        let k = radiator_conductance(power, 1.0);
        let mut t = START_C;
        for _ in 0..(240 * 60 * 10) {
            // A third less air through the radiator in 38 °C.
            step(
                &mut t,
                REFERENCE_POWER_SHARE * power,
                40.0,
                38.0,
                k,
                1.0 / 240.0,
            );
        }
        assert!(t > OVERHEAT_C, "{t}");
        assert!(power_factor(t) < 1.0);
        assert_eq!(power_factor(TARGET_C), 1.0);
        assert_eq!(power_factor(500.0), OVERHEAT_FLOOR);
    }
}
