//! Per-driver car setup: the garage screen's knobs.
//!
//! A setup is a handful of *clicks* away from the car's `car.toml`, one
//! `i8` per knob, so the wire and the client need none of the car's base
//! figures: the client shows "Front springs +2 (+8%)" and the server turns
//! that into newtons per metre against the file it loaded. Every knob has a
//! fixed click range (`KNOBS`) and a fixed effect per click; `clamp` pins a
//! request into range and `apply` bakes the result into a tuned copy of the
//! `CarConfig`, made once per change and looked up per tick in place of the
//! shared config (`GameSession::tuned_config`), so the hot loop is untouched.
//!
//! Only knobs the simulation actually reads are offered: there is no
//! differential model and rolling resistance is never consumed, so neither
//! appears here. Tyre pressure is the one addition to the physics — a
//! per-axle grip factor off the optimum (`TireConfig::pressure_grip_factor`)
//! — which is what lets pressures shift the balance of the car.

use crate::data::CarConfig;
use serde::{Deserialize, Serialize};

/// Most clicks either side of the file's value for a symmetric knob.
pub const MAX_CLICKS: i8 = 5;

/// One knob of the setup: its wire name and click range.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Knob {
    pub name: &'static str,
    pub min: i8,
    pub max: i8,
}

/// kPa per tyre-pressure click.
pub const TYRE_PRESSURE_KPA_PER_CLICK: f32 = 5.0;
/// Revs the limiter (and the redline it is tied to) drops per click; the
/// knob only lowers it, a file's limiter is the engine's ceiling.
pub const REV_LIMITER_RPM_PER_CLICK: f32 = 100.0;
/// Engine braking torque scale per click.
pub const ENGINE_BRAKING_PER_CLICK: f32 = 0.15;
/// Final drive ratio scale per click; positive is shorter gearing.
pub const FINAL_DRIVE_PER_CLICK: f32 = 0.02;
/// Top gear ratio scale per click, blended linearly down the ladder so
/// first is untouched; positive closes the gears up (a shorter top).
pub const GEAR_SPREAD_PER_CLICK: f32 = 0.02;
/// Drive torque scale per click of the torque map; only lowers.
pub const TORQUE_MAP_PER_CLICK: f32 = 0.04;
/// Front brake share per click, as an absolute fraction of the total.
pub const BRAKE_BIAS_PER_CLICK: f32 = 0.01;
/// Spring rate scale per click.
pub const SPRING_PER_CLICK: f32 = 0.04;
/// Damper (bump and rebound together) scale per click.
pub const DAMPER_PER_CLICK: f32 = 0.05;
/// Anti-roll bar stiffness scale per click.
pub const ANTI_ROLL_PER_CLICK: f32 = 0.08;

/// The knobs in the order the garage screen shows them, grouped tyres,
/// engine, transmission, torque, suspension.
pub const KNOBS: [Knob; 14] = [
    Knob {
        name: "tyre_pressure_front",
        min: -MAX_CLICKS,
        max: MAX_CLICKS,
    },
    Knob {
        name: "tyre_pressure_rear",
        min: -MAX_CLICKS,
        max: MAX_CLICKS,
    },
    Knob {
        name: "rev_limiter",
        min: -MAX_CLICKS,
        max: 0,
    },
    Knob {
        name: "engine_braking",
        min: -MAX_CLICKS,
        max: MAX_CLICKS,
    },
    Knob {
        name: "final_drive",
        min: -MAX_CLICKS,
        max: MAX_CLICKS,
    },
    Knob {
        name: "gear_spread",
        min: -MAX_CLICKS,
        max: MAX_CLICKS,
    },
    Knob {
        name: "torque_map",
        min: -MAX_CLICKS,
        max: 0,
    },
    Knob {
        name: "brake_bias",
        min: -MAX_CLICKS,
        max: MAX_CLICKS,
    },
    Knob {
        name: "spring_front",
        min: -MAX_CLICKS,
        max: MAX_CLICKS,
    },
    Knob {
        name: "spring_rear",
        min: -MAX_CLICKS,
        max: MAX_CLICKS,
    },
    Knob {
        name: "damper_front",
        min: -MAX_CLICKS,
        max: MAX_CLICKS,
    },
    Knob {
        name: "damper_rear",
        min: -MAX_CLICKS,
        max: MAX_CLICKS,
    },
    Knob {
        name: "anti_roll_front",
        min: -MAX_CLICKS,
        max: MAX_CLICKS,
    },
    Knob {
        name: "anti_roll_rear",
        min: -MAX_CLICKS,
        max: MAX_CLICKS,
    },
];

/// A driver's setup as clicks per knob; all zero is the car as filed.
/// Every field defaults so an older client, or a partial message, leaves
/// the knobs it does not name at the file's value.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Serialize, Deserialize)]
pub struct CarSetup {
    #[serde(default)]
    pub tyre_pressure_front: i8,
    #[serde(default)]
    pub tyre_pressure_rear: i8,
    #[serde(default)]
    pub rev_limiter: i8,
    #[serde(default)]
    pub engine_braking: i8,
    #[serde(default)]
    pub final_drive: i8,
    #[serde(default)]
    pub gear_spread: i8,
    #[serde(default)]
    pub torque_map: i8,
    #[serde(default)]
    pub brake_bias: i8,
    #[serde(default)]
    pub spring_front: i8,
    #[serde(default)]
    pub spring_rear: i8,
    #[serde(default)]
    pub damper_front: i8,
    #[serde(default)]
    pub damper_rear: i8,
    #[serde(default)]
    pub anti_roll_front: i8,
    #[serde(default)]
    pub anti_roll_rear: i8,
}

impl CarSetup {
    /// The knob values in `KNOBS` order.
    pub fn clicks(&self) -> [i8; 14] {
        [
            self.tyre_pressure_front,
            self.tyre_pressure_rear,
            self.rev_limiter,
            self.engine_braking,
            self.final_drive,
            self.gear_spread,
            self.torque_map,
            self.brake_bias,
            self.spring_front,
            self.spring_rear,
            self.damper_front,
            self.damper_rear,
            self.anti_roll_front,
            self.anti_roll_rear,
        ]
    }

    /// A setup from knob values in `KNOBS` order.
    pub fn from_clicks(c: [i8; 14]) -> Self {
        Self {
            tyre_pressure_front: c[0],
            tyre_pressure_rear: c[1],
            rev_limiter: c[2],
            engine_braking: c[3],
            final_drive: c[4],
            gear_spread: c[5],
            torque_map: c[6],
            brake_bias: c[7],
            spring_front: c[8],
            spring_rear: c[9],
            damper_front: c[10],
            damper_rear: c[11],
            anti_roll_front: c[12],
            anti_roll_rear: c[13],
        }
    }

    /// Every knob pinned into its range.
    pub fn clamp(self) -> Self {
        let mut clicks = self.clicks();
        for (value, knob) in clicks.iter_mut().zip(KNOBS.iter()) {
            *value = (*value).clamp(knob.min, knob.max);
        }
        Self::from_clicks(clicks)
    }

    /// True when every knob is at the file's value, so the shared config
    /// serves and no tuned copy is needed.
    pub fn is_stock(&self) -> bool {
        self.clicks().iter().all(|&c| c == 0)
    }

    /// The car with this setup applied. `self` is expected clamped.
    pub fn apply(&self, base: &CarConfig) -> CarConfig {
        let scale = |clicks: i8, per_click: f32| 1.0 + clicks as f32 * per_click;
        let mut car = base.clone();

        // Tyres: pressure either side of the file's optimum.
        car.tire_config.pressure_front_kpa = base.tire_config.pressure_front_kpa
            + self.tyre_pressure_front as f32 * TYRE_PRESSURE_KPA_PER_CLICK;
        car.tire_config.pressure_rear_kpa = base.tire_config.pressure_rear_kpa
            + self.tyre_pressure_rear as f32 * TYRE_PRESSURE_KPA_PER_CLICK;

        // Engine: the limiter is `rev_limiter_rpm.max(redline_rpm)` in the
        // sim, so both come down together and stay above idle.
        let drop = -(self.rev_limiter as f32) * REV_LIMITER_RPM_PER_CLICK;
        let floor = base.idle_rpm + 500.0;
        car.redline_rpm = (base.redline_rpm - drop).max(floor);
        car.engine.rev_limiter_rpm = (base.engine.rev_limiter_rpm - drop).max(floor);
        car.engine.engine_brake_torque_nm = base.engine.engine_brake_torque_nm
            * scale(self.engine_braking, ENGINE_BRAKING_PER_CLICK);

        // Transmission: the final drive scales every gear; the spread
        // scales the top gear fully, first not at all, the rest by where
        // they sit in the ladder. Reverse (index 0) follows first, untouched.
        car.final_drive_ratio =
            base.final_drive_ratio * scale(self.final_drive, FINAL_DRIVE_PER_CLICK);
        let forward = base.gear_ratios.len().saturating_sub(1);
        if forward >= 2 {
            let top_scale = scale(self.gear_spread, GEAR_SPREAD_PER_CLICK);
            for (i, ratio) in car.gear_ratios.iter_mut().enumerate().skip(2) {
                let t = (i - 1) as f32 / (forward - 1) as f32;
                *ratio = base.gear_ratios[i] * (1.0 + (top_scale - 1.0) * t);
            }
        }

        // Torque: how much of the curve the driver gets, and where the
        // brakes put theirs.
        let map = scale(self.torque_map, TORQUE_MAP_PER_CLICK);
        for point in car.engine.torque_curve.iter_mut() {
            point.torque_nm *= map;
        }
        car.max_engine_torque_nm = base.max_engine_torque_nm * map;
        car.max_engine_power_w = base.max_engine_power_w * map;
        car.brake_bias_front = (base.brake_bias_front
            + self.brake_bias as f32 * BRAKE_BIAS_PER_CLICK)
            .clamp(0.05, 0.95);

        // Suspension.
        let s = &mut car.suspension;
        let b = &base.suspension;
        s.spring_rate_front_n_per_m =
            b.spring_rate_front_n_per_m * scale(self.spring_front, SPRING_PER_CLICK);
        s.spring_rate_rear_n_per_m =
            b.spring_rate_rear_n_per_m * scale(self.spring_rear, SPRING_PER_CLICK);
        let df = scale(self.damper_front, DAMPER_PER_CLICK);
        let dr = scale(self.damper_rear, DAMPER_PER_CLICK);
        s.damper_compression_front = b.damper_compression_front * df;
        s.damper_rebound_front = b.damper_rebound_front * df;
        s.damper_compression_rear = b.damper_compression_rear * dr;
        s.damper_rebound_rear = b.damper_rebound_rear * dr;
        s.anti_roll_bar_front =
            b.anti_roll_bar_front * scale(self.anti_roll_front, ANTI_ROLL_PER_CLICK);
        s.anti_roll_bar_rear =
            b.anti_roll_bar_rear * scale(self.anti_roll_rear, ANTI_ROLL_PER_CLICK);

        car
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::data::TorqueCurvePoint;

    fn car() -> CarConfig {
        let mut car = CarConfig::default();
        car.engine.torque_curve = vec![
            TorqueCurvePoint {
                rpm: 2000.0,
                torque_nm: 300.0,
            },
            TorqueCurvePoint {
                rpm: 6000.0,
                torque_nm: 450.0,
            },
        ];
        car
    }

    #[test]
    fn stock_setup_is_the_file() {
        let base = car();
        let tuned = CarSetup::default().apply(&base);
        assert!(CarSetup::default().is_stock());
        assert_eq!(tuned.gear_ratios, base.gear_ratios);
        assert_eq!(tuned.final_drive_ratio, base.final_drive_ratio);
        assert_eq!(tuned.brake_bias_front, base.brake_bias_front);
        assert_eq!(
            tuned.suspension.spring_rate_front_n_per_m,
            base.suspension.spring_rate_front_n_per_m
        );
        assert_eq!(
            tuned.tire_config.pressure_front_kpa,
            base.tire_config.pressure_front_kpa
        );
        assert_eq!(tuned.redline_rpm, base.redline_rpm);
    }

    #[test]
    fn clamp_pins_every_knob_and_one_sided_knobs_only_lower() {
        let wild = CarSetup::from_clicks([100, -100, 3, 9, -9, 7, 4, 6, 8, -8, 20, -20, 6, -6]);
        let c = wild.clamp();
        assert_eq!(c.tyre_pressure_front, MAX_CLICKS);
        assert_eq!(c.tyre_pressure_rear, -MAX_CLICKS);
        assert_eq!(c.rev_limiter, 0, "the limiter cannot be raised");
        assert_eq!(c.torque_map, 0, "the torque map cannot exceed the file");
        assert_eq!(c.engine_braking, MAX_CLICKS);
        assert_eq!(c.final_drive, -MAX_CLICKS);
        assert_eq!(c.anti_roll_rear, -MAX_CLICKS);
        assert_eq!(c.clamp(), c);
        assert_eq!(KNOBS.len(), c.clicks().len());
    }

    #[test]
    fn gear_spread_scales_top_fully_and_leaves_first_and_reverse() {
        let base = car();
        let setup = CarSetup {
            gear_spread: 5,
            ..Default::default()
        };
        let tuned = setup.apply(&base);
        let n = base.gear_ratios.len();
        assert_eq!(tuned.gear_ratios[0], base.gear_ratios[0]);
        assert_eq!(tuned.gear_ratios[1], base.gear_ratios[1]);
        let top = tuned.gear_ratios[n - 1] / base.gear_ratios[n - 1];
        assert!((top - 1.10).abs() < 1e-5, "top scaled by 10%, got {top}");
        // Still a descending ladder: the loader's rule survives every setup.
        for w in tuned.gear_ratios[1..].windows(2) {
            assert!(w[0] > w[1], "{:?}", tuned.gear_ratios);
        }
    }

    #[test]
    fn engine_knobs_lower_the_limiter_and_scale_the_curve() {
        let base = car();
        let setup = CarSetup {
            rev_limiter: -3,
            torque_map: -5,
            engine_braking: -5,
            ..Default::default()
        };
        let tuned = setup.apply(&base);
        assert_eq!(tuned.redline_rpm, base.redline_rpm - 300.0);
        assert_eq!(
            tuned.engine.rev_limiter_rpm,
            base.engine.rev_limiter_rpm - 300.0
        );
        assert!((tuned.engine.torque_curve[1].torque_nm - 450.0 * 0.8).abs() < 1e-3);
        assert!((tuned.max_engine_power_w - base.max_engine_power_w * 0.8).abs() < 1.0);
        assert!(
            (tuned.engine.engine_brake_torque_nm - base.engine.engine_brake_torque_nm * 0.25).abs()
                < 1e-3
        );
        // A limiter click can never push the cut below idle.
        let mut low = base.clone();
        low.redline_rpm = low.idle_rpm + 200.0;
        low.engine.rev_limiter_rpm = low.idle_rpm + 200.0;
        let tuned = CarSetup {
            rev_limiter: -5,
            ..Default::default()
        }
        .apply(&low);
        assert!(tuned.redline_rpm > low.idle_rpm);
    }

    #[test]
    fn chassis_knobs_scale_their_fields() {
        let base = car();
        let setup = CarSetup {
            tyre_pressure_front: 2,
            tyre_pressure_rear: -1,
            brake_bias: 3,
            spring_front: 1,
            spring_rear: -1,
            damper_front: 2,
            damper_rear: -2,
            anti_roll_front: 1,
            anti_roll_rear: -1,
            ..Default::default()
        };
        let t = setup.apply(&base);
        let b = &base.suspension;
        assert_eq!(
            t.tire_config.pressure_front_kpa,
            base.tire_config.pressure_front_kpa + 10.0
        );
        assert_eq!(
            t.tire_config.pressure_rear_kpa,
            base.tire_config.pressure_rear_kpa - 5.0
        );
        assert!((t.brake_bias_front - (base.brake_bias_front + 0.03)).abs() < 1e-6);
        assert!(
            (t.suspension.spring_rate_front_n_per_m - b.spring_rate_front_n_per_m * 1.04).abs()
                < 1e-2
        );
        assert!(
            (t.suspension.spring_rate_rear_n_per_m - b.spring_rate_rear_n_per_m * 0.96).abs()
                < 1e-2
        );
        assert!(
            (t.suspension.damper_compression_front - b.damper_compression_front * 1.10).abs()
                < 1e-2
        );
        assert!((t.suspension.damper_rebound_front - b.damper_rebound_front * 1.10).abs() < 1e-2);
        assert!((t.suspension.damper_rebound_rear - b.damper_rebound_rear * 0.90).abs() < 1e-2);
        assert!((t.suspension.anti_roll_bar_front - b.anti_roll_bar_front * 1.08).abs() < 1e-2);
        assert!((t.suspension.anti_roll_bar_rear - b.anti_roll_bar_rear * 0.92).abs() < 1e-2);
    }

    #[test]
    fn partial_message_leaves_unnamed_knobs_stock() {
        let json = r#"{"spring_front": 2}"#;
        let setup: CarSetup = serde_json::from_str(json).unwrap();
        assert_eq!(
            setup,
            CarSetup {
                spring_front: 2,
                ..Default::default()
            }
        );
    }
}
