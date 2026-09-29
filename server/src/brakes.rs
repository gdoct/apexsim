//! Brake temperature and fade.
//!
//! Each corner's brake is one thermal mass (disc and pads), heated by the
//! power the brake absorbs, its force times the speed its wheel turns at,
//! so a locked wheel heats its tyre, not its disc. It is cooled by the air
//! through its duct (growing with the car's speed; the setup's
//! `brake_ducts` scales it) and, hot, by radiation.
//!
//! What the pads grip with depends on the temperature and the material
//! ([`BrakeMaterial::friction`]):
//!
//! - **carbon** (F1, prototypes): weak cold, full between 400 and 900 °C,
//!   fading past it. A car on cold carbon brakes stops a good deal later.
//! - **steel** (GT3): near full from cold, full to 600 °C, fading past 700.
//!
//! The physics scales each wheel's brake force by its friction
//! (`physics::update_car_3d`); the AI brakes earlier on brakes that are
//! not at their best (`CarState::brake_share`). Cars go out on brakes at
//! the air (the garage), warmed a formation lap's worth (a race grid) or in
//! their window (a hotlap), like the tyres.

use serde::{Deserialize, Serialize};

use crate::data::CarState;

/// What a brake is made of.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum BrakeMaterial {
    Carbon,
    #[default]
    Steel,
}

/// A driver on carbon brakes counts on them coming in within the first
/// moment of a stop: the AI judges them at their temperature plus this, °C
/// (`CarState::brake_share`), or it would brake early at every corner.
const CARBON_STOP_RISE_C: f32 = 100.0;
/// Radiating area and emissivity of a disc (both faces).
const RADIATING_AREA_M2: f32 = 0.12;
const EMISSIVITY: f32 = 0.85;
const STEFAN_BOLTZMANN: f32 = 5.67e-8;
const KELVIN: f32 = 273.15;
/// A formation lap warms carbon brakes to this, °C (steel: the air and
/// this share of the way to the window).
const GRID_CARBON_C: f32 = 300.0;

impl BrakeMaterial {
    /// The material a class races on when its car.toml does not say.
    pub fn for_class(class: &str) -> BrakeMaterial {
        match class {
            "F1" | "Hypercar" | "LMP2" | "LMP1" | "LMP" => BrakeMaterial::Carbon,
            _ => BrakeMaterial::Steel,
        }
    }

    /// Disc and pads' heat capacity, J/K: a carbon disc is light, a steel
    /// one heavy.
    pub fn heat_capacity(self) -> f32 {
        match self {
            BrakeMaterial::Carbon => 1_000.0,
            BrakeMaterial::Steel => 5_500.0,
        }
    }

    /// Convection from the disc through its duct: still air, and per m/s,
    /// W/K. A carbon disc is small and runs on a small duct.
    fn convection(self) -> (f32, f32) {
        match self {
            BrakeMaterial::Carbon => (2.0, 1.0),
            BrakeMaterial::Steel => (4.0, 2.2),
        }
    }

    /// Where the pads grip fully, °C.
    pub fn window_c(self) -> (f32, f32) {
        match self {
            BrakeMaterial::Carbon => (400.0, 900.0),
            BrakeMaterial::Steel => (200.0, 600.0),
        }
    }

    /// The pads' grip at `temperature_c`, as a share of their best.
    pub fn friction(self, temperature_c: f32) -> f32 {
        let table: &[(f32, f32)] = match self {
            BrakeMaterial::Carbon => &[
                (0.0, 0.55),
                (150.0, 0.62),
                (300.0, 0.85),
                (400.0, 1.0),
                (900.0, 1.0),
                (1050.0, 0.85),
                (1200.0, 0.6),
            ],
            BrakeMaterial::Steel => &[
                (0.0, 0.9),
                (100.0, 0.96),
                (200.0, 1.0),
                (600.0, 1.0),
                (700.0, 0.88),
                (850.0, 0.55),
            ],
        };
        interpolate(table, temperature_c)
    }
}

fn interpolate(table: &[(f32, f32)], x: f32) -> f32 {
    let (x0, y0) = table[0];
    if x <= x0 {
        return y0;
    }
    for pair in table.windows(2) {
        let ((a, ya), (b, yb)) = (pair[0], pair[1]);
        if x <= b {
            return ya + (yb - ya) * (x - a) / (b - a);
        }
    }
    table[table.len() - 1].1
}

/// What a car's brakes are at when it goes out: the air from the garage,
/// warmed by a formation lap on a race grid, or in their window for a
/// hotlap.
pub fn start_temperature_c(material: BrakeMaterial, air_c: f32, grid: bool, warm: bool) -> f32 {
    let (low, _) = material.window_c();
    if warm {
        return low + 50.0;
    }
    if !grid {
        return air_c;
    }
    match material {
        BrakeMaterial::Carbon => GRID_CARBON_C.max(air_c),
        BrakeMaterial::Steel => air_c + 0.5 * (low - air_c).max(0.0),
    }
}

/// Put all four brakes at `temperature_c`.
pub fn fit(state: &mut CarState, temperature_c: f32) {
    state.brake_temp_c = [temperature_c; 4];
}

/// Advance one brake by `dt`: `power_w` in, the air (through a duct
/// `duct_scale` times the car's own) and radiation out.
pub fn step(
    temperature_c: &mut f32,
    power_w: f32,
    speed_mps: f32,
    air_c: f32,
    material: BrakeMaterial,
    duct_scale: f32,
    dt: f32,
) {
    let t = *temperature_c;
    let (still, forced) = material.convection();
    let convection = duct_scale.max(0.0) * (still + forced * speed_mps.abs()) * (t - air_c);
    let (tk, ak) = (t + KELVIN, air_c + KELVIN);
    let radiation = EMISSIVITY * STEFAN_BOLTZMANN * RADIATING_AREA_M2 * (tk.powi(4) - ak.powi(4));
    *temperature_c =
        t + (power_w.max(0.0) - convection - radiation) * dt / material.heat_capacity();
}

impl CarState {
    /// The worst brake's grip as a share of its best, as a driver who can
    /// feel the pedal expects it in the stop to come: what the AI goes long
    /// by. Carbon is judged hotter than it is ([`CARBON_STOP_RISE_C`]). 1.0
    /// before the brakes are fitted.
    pub fn brake_share(&self, material: BrakeMaterial) -> f32 {
        if !self.tyres_fitted {
            return 1.0;
        }
        let rise = match material {
            BrakeMaterial::Carbon => CARBON_STOP_RISE_C,
            BrakeMaterial::Steel => 0.0,
        };
        self.brake_temp_c
            .iter()
            .map(|t| material.friction(*t + rise))
            .fold(1.0, f32::min)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn carbon_is_weak_cold_and_steel_is_not() {
        let carbon = BrakeMaterial::Carbon;
        let steel = BrakeMaterial::Steel;
        assert!(carbon.friction(20.0) < 0.6);
        assert!(steel.friction(20.0) > 0.88);
        assert_eq!(carbon.friction(600.0), 1.0);
        assert_eq!(steel.friction(400.0), 1.0);
        assert!(carbon.friction(1100.0) < 0.85);
        assert!(steel.friction(800.0) < 0.7, "steel fades far sooner");
        assert_eq!(BrakeMaterial::for_class("F1"), carbon);
        assert_eq!(BrakeMaterial::for_class("GT3"), steel);
    }

    #[test]
    fn a_stop_heats_a_carbon_disc_hundreds_of_degrees_and_a_straight_cools_it() {
        // An F1 front brake taking a quarter of 3 MJ over two seconds.
        let mut t = 450.0;
        for _ in 0..480 {
            step(
                &mut t,
                0.75e6 / 2.0,
                60.0,
                20.0,
                BrakeMaterial::Carbon,
                1.0,
                1.0 / 240.0,
            );
        }
        assert!(t > 700.0, "{t}");
        let peak = t;
        for _ in 0..(240 * 8) {
            step(
                &mut t,
                0.0,
                85.0,
                20.0,
                BrakeMaterial::Carbon,
                1.0,
                1.0 / 240.0,
            );
        }
        assert!(t < peak - 250.0, "eight seconds of straight: {peak} -> {t}");
        // Bigger ducts cool it faster.
        let (mut small, mut big) = (peak, peak);
        for _ in 0..(240 * 4) {
            step(
                &mut small,
                0.0,
                85.0,
                20.0,
                BrakeMaterial::Carbon,
                0.5,
                1.0 / 240.0,
            );
            step(
                &mut big,
                0.0,
                85.0,
                20.0,
                BrakeMaterial::Carbon,
                1.5,
                1.0 / 240.0,
            );
        }
        assert!(big < small - 50.0);
    }

    #[test]
    fn a_grid_warms_carbon_and_a_hotlap_starts_in_the_window() {
        let carbon = BrakeMaterial::Carbon;
        assert_eq!(start_temperature_c(carbon, 20.0, false, false), 20.0);
        assert_eq!(
            start_temperature_c(carbon, 20.0, true, false),
            GRID_CARBON_C
        );
        let warm = start_temperature_c(carbon, 20.0, false, true);
        assert_eq!(carbon.friction(warm), 1.0);
    }
}
