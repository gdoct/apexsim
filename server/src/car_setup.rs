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
//!
//! The aero knobs (appended after the fuel load) scale each axle's lift
//! coefficient, with drag, and move the static ride heights on the car's
//! aero map (`crate::aero`), whose reference stays the file's; a car with
//! no `[aero]` table takes the wings but not the ride heights.
//!
//! The fuel knob is the odd one out: it is not a figure of the car but
//! laps of fuel either side of what the session fills it with
//! (`GameSession::start_fuel_liters`), so `apply` leaves it alone and it
//! only takes effect where a car is fuelled — never mid-lap.

use crate::data::{CarConfig, TireConfig};
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
/// Laps of fuel per click of the fuel load, on the session's own fill.
pub const FUEL_LAPS_PER_CLICK: f32 = 1.0;
/// Scale of a wing's lift coefficient per click.
pub const WING_PER_CLICK: f32 = 0.05;
/// Drag scale per click of the front and of the rear wing: the rear wing
/// costs three times as much drag for its downforce.
pub const FRONT_WING_DRAG_PER_CLICK: f32 = 0.005;
pub const REAR_WING_DRAG_PER_CLICK: f32 = 0.015;
/// Brake duct size per click, as a share of the car's own; and the drag
/// each click of opening costs. Per axle: `brake_ducts` is the front pair,
/// `brake_ducts_rear` the rear.
pub const BRAKE_DUCT_PER_CLICK: f32 = 0.1;
pub const BRAKE_DUCT_DRAG_PER_CLICK: f32 = 0.003;
/// Radiator inlet per click, as a share of the car's own (`[engine]
/// radiator_scale`), and the drag each click of opening costs.
pub const RADIATOR_PER_CLICK: f32 = 0.08;
pub const RADIATOR_DRAG_PER_CLICK: f32 = 0.003;
/// Static ride height per click, m (`crate::aero`).
pub const RIDE_HEIGHT_M_PER_CLICK: f32 = 0.002;
/// Camber per click, degrees: a click more is more negative camber
/// (`crate::geometry`; only on a car.toml that gives camber).
pub const CAMBER_DEG_PER_CLICK: f32 = -0.25;
/// Toe per wheel per click, degrees: a click more is more toe-in.
pub const TOE_DEG_PER_CLICK: f32 = 0.05;
/// Lowest static ride height a setup can ask for, m.
const MIN_RIDE_HEIGHT_M: f32 = 0.01;

/// Number of knobs in a setup.
pub const KNOB_COUNT: usize = 28;

/// The knobs in wire order: tyres, engine, transmission, torque,
/// suspension, the fuel load, then the aero (each group appended after
/// the last, so the order of the others never moved).
pub const KNOBS: [Knob; KNOB_COUNT] = [
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
    Knob {
        name: "fuel_load",
        min: -MAX_CLICKS,
        max: MAX_CLICKS,
    },
    Knob {
        name: "front_wing",
        min: -MAX_CLICKS,
        max: MAX_CLICKS,
    },
    Knob {
        name: "rear_wing",
        min: -MAX_CLICKS,
        max: MAX_CLICKS,
    },
    Knob {
        name: "ride_height_front",
        min: -MAX_CLICKS,
        max: MAX_CLICKS,
    },
    Knob {
        name: "ride_height_rear",
        min: -MAX_CLICKS,
        max: MAX_CLICKS,
    },
    Knob {
        name: "tyre_compound",
        min: -3,
        max: 1,
    },
    Knob {
        name: "brake_ducts",
        min: -MAX_CLICKS,
        max: MAX_CLICKS,
    },
    Knob {
        name: "camber_front",
        min: -MAX_CLICKS,
        max: MAX_CLICKS,
    },
    Knob {
        name: "camber_rear",
        min: -MAX_CLICKS,
        max: MAX_CLICKS,
    },
    Knob {
        name: "toe_front",
        min: -MAX_CLICKS,
        max: MAX_CLICKS,
    },
    Knob {
        name: "toe_rear",
        min: -MAX_CLICKS,
        max: MAX_CLICKS,
    },
    Knob {
        name: "brake_ducts_rear",
        min: -MAX_CLICKS,
        max: MAX_CLICKS,
    },
    Knob {
        name: "brake_pads",
        min: -1,
        max: 1,
    },
    Knob {
        name: "radiator",
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
    /// Laps of fuel over (or under) the session's fill.
    #[serde(default)]
    pub fuel_load: i8,
    /// Wing angle per axle: more downforce, and drag, per click.
    #[serde(default)]
    pub front_wing: i8,
    #[serde(default)]
    pub rear_wing: i8,
    /// Static ride height per axle, [`RIDE_HEIGHT_M_PER_CLICK`] a click;
    /// it only matters to a car with an `[aero]` map.
    #[serde(default)]
    pub ride_height_front: i8,
    #[serde(default)]
    pub ride_height_rear: i8,
    /// The compound the next set of tyres is: -1 hard, 0 medium, +1 soft.
    /// Like the fuel load it is not a figure of the car: a set is fitted
    /// in the garage, on the grid or at a pit stop, never mid-lap.
    #[serde(default)]
    pub tyre_compound: i8,
    /// Brake ducts: more air to the brakes (cooler, a little drag), or
    /// less (warmer, for a cold day or carbon brakes that will not come in).
    #[serde(default)]
    pub brake_ducts: i8,
    /// Static camber per axle, [`CAMBER_DEG_PER_CLICK`] a click (more
    /// negative per click), and toe per axle, [`TOE_DEG_PER_CLICK`] a
    /// click (more toe-in per click): `crate::geometry`.
    #[serde(default)]
    pub camber_front: i8,
    #[serde(default)]
    pub camber_rear: i8,
    #[serde(default)]
    pub toe_front: i8,
    #[serde(default)]
    pub toe_rear: i8,
    /// The rear brake ducts (`brake_ducts` is the front pair).
    #[serde(default)]
    pub brake_ducts_rear: i8,
    /// The pads: -1 endurance, 0 standard, +1 sprint (`brakes::BrakePads`).
    /// Like the compound, fitted at a stop or in the garage, never mid-lap.
    #[serde(default)]
    pub brake_pads: i8,
    /// The radiator inlet, [`RADIATOR_PER_CLICK`] a click: cooler for drag.
    #[serde(default)]
    pub radiator: i8,
}

impl CarSetup {
    /// The knob values in `KNOBS` order.
    pub fn clicks(&self) -> [i8; KNOB_COUNT] {
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
            self.fuel_load,
            self.front_wing,
            self.rear_wing,
            self.ride_height_front,
            self.ride_height_rear,
            self.tyre_compound,
            self.brake_ducts,
            self.camber_front,
            self.camber_rear,
            self.toe_front,
            self.toe_rear,
            self.brake_ducts_rear,
            self.brake_pads,
            self.radiator,
        ]
    }

    /// A setup from knob values in `KNOBS` order.
    pub fn from_clicks(c: [i8; KNOB_COUNT]) -> Self {
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
            fuel_load: c[14],
            front_wing: c[15],
            rear_wing: c[16],
            ride_height_front: c[17],
            ride_height_rear: c[18],
            tyre_compound: c[19],
            brake_ducts: c[20],
            camber_front: c[21],
            camber_rear: c[22],
            toe_front: c[23],
            toe_rear: c[24],
            brake_ducts_rear: c[25],
            brake_pads: c[26],
            radiator: c[27],
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

    /// True when every knob is at the file's value.
    pub fn is_stock(&self) -> bool {
        self.clicks().iter().all(|&c| c == 0)
    }

    /// True when a knob `apply` bakes into the car is off the file's value,
    /// so a tuned copy is needed; the fuel load is not one.
    pub fn changes_car(&self) -> bool {
        Self {
            fuel_load: 0,
            tyre_compound: 0,
            ..*self
        } != Self::default()
    }

    /// The compound the next set is on a dry road, as an index into the
    /// car's compounds (`TireConfig::compound_for_click`): the reference at
    /// 0, softer for positive clicks, harder and then the treaded tyres for
    /// negative (on the default list +1 soft, 0 medium, -1 hard, -2
    /// intermediate, -3 wet).
    pub fn compound_index(&self, tire: &TireConfig) -> u8 {
        tire.compound_for_click(self.tyre_compound)
    }

    /// The compound the next set is on a road with this much `water`
    /// (`TrackSurface::water`): the stock pick (the reference) means "the
    /// tyre for the weather", so a driver who never opened the garage is
    /// not sent out on slicks in the rain. Any other pick is honoured.
    pub fn compound_index_for(&self, tire: &TireConfig, water: f32) -> u8 {
        if self.tyre_compound == 0 {
            if let Some(wet) = tire.weather_compound(water) {
                return wet;
            }
        }
        self.compound_index(tire)
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

        // Aero: each wing scales its axle's downforce and the drag with it;
        // the ride heights move the car on its aero map, whose reference
        // stays where the file put it.
        car.lift_coefficient_front =
            base.lift_coefficient_front * scale(self.front_wing, WING_PER_CLICK);
        car.lift_coefficient_rear =
            base.lift_coefficient_rear * scale(self.rear_wing, WING_PER_CLICK);
        car.drag_coefficient = base.drag_coefficient
            * scale(self.front_wing, FRONT_WING_DRAG_PER_CLICK)
            * scale(self.rear_wing, REAR_WING_DRAG_PER_CLICK)
            * scale(self.brake_ducts, BRAKE_DUCT_DRAG_PER_CLICK)
            * scale(self.brake_ducts_rear, BRAKE_DUCT_DRAG_PER_CLICK)
            * scale(self.radiator, RADIATOR_DRAG_PER_CLICK);
        car.brake_duct_scale =
            base.brake_duct_scale * scale(self.brake_ducts, BRAKE_DUCT_PER_CLICK);
        car.brake_duct_scale_rear =
            base.brake_duct_scale_rear * scale(self.brake_ducts_rear, BRAKE_DUCT_PER_CLICK);
        car.brake_pads = crate::brakes::BrakePads::from_click(self.brake_pads);
        car.engine.radiator_scale =
            base.engine.radiator_scale * scale(self.radiator, RADIATOR_PER_CLICK);
        let height = |base_m: f32, clicks: i8| {
            (base_m + clicks as f32 * RIDE_HEIGHT_M_PER_CLICK).max(MIN_RIDE_HEIGHT_M)
        };
        car.aero.ride_height_front_m =
            height(base.aero.ride_height_front_m, self.ride_height_front);
        car.aero.ride_height_rear_m = height(base.aero.ride_height_rear_m, self.ride_height_rear);

        // Geometry: the static camber and toe move; the filed camber the
        // grip is measured against does not (`crate::geometry`).
        let s = &mut car.suspension;
        let b = &base.suspension;
        s.camber_front_deg =
            (b.camber_front_deg + self.camber_front as f32 * CAMBER_DEG_PER_CLICK).clamp(-8.0, 3.0);
        s.camber_rear_deg =
            (b.camber_rear_deg + self.camber_rear as f32 * CAMBER_DEG_PER_CLICK).clamp(-8.0, 3.0);
        s.toe_front_deg = b.toe_front_deg + self.toe_front as f32 * TOE_DEG_PER_CLICK;
        s.toe_rear_deg = b.toe_rear_deg + self.toe_rear as f32 * TOE_DEG_PER_CLICK;

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
        let wild = CarSetup::from_clicks([
            100, -100, 3, 9, -9, 7, 4, 6, 8, -8, 20, -20, 6, -6, 30, 9, -9, 12, -12, 4, -9, 7, -7,
            9, -9, 8, 3, -7,
        ]);
        let c = wild.clamp();
        assert_eq!(c.brake_ducts_rear, MAX_CLICKS);
        assert_eq!(c.brake_pads, 1, "sprint is the end of the pad range");
        assert_eq!(c.radiator, -MAX_CLICKS);
        assert_eq!(c.tyre_pressure_front, MAX_CLICKS);
        assert_eq!(c.tyre_pressure_rear, -MAX_CLICKS);
        assert_eq!(c.rev_limiter, 0, "the limiter cannot be raised");
        assert_eq!(c.torque_map, 0, "the torque map cannot exceed the file");
        assert_eq!(c.engine_braking, MAX_CLICKS);
        assert_eq!(c.final_drive, -MAX_CLICKS);
        assert_eq!(c.anti_roll_rear, -MAX_CLICKS);
        assert_eq!(c.fuel_load, MAX_CLICKS);
        assert_eq!(c.tyre_compound, 1, "soft is the end of the range");
        assert_eq!(
            c.compound_index(&TireConfig::default()),
            0,
            "and the first compound"
        );
        assert_eq!(c.brake_ducts, -MAX_CLICKS);
        assert_eq!((c.camber_front, c.camber_rear), (MAX_CLICKS, -MAX_CLICKS));
        assert_eq!((c.toe_front, c.toe_rear), (MAX_CLICKS, -MAX_CLICKS));
        assert_eq!((c.front_wing, c.rear_wing), (MAX_CLICKS, -MAX_CLICKS));
        assert_eq!(
            (c.ride_height_front, c.ride_height_rear),
            (MAX_CLICKS, -MAX_CLICKS)
        );
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
    fn the_fuel_knob_is_not_a_figure_of_the_car() {
        let fuel_only = CarSetup {
            fuel_load: -2,
            ..Default::default()
        };
        assert!(!fuel_only.is_stock(), "the knob is kept");
        assert!(!fuel_only.changes_car(), "but needs no tuned copy");
        let softs = CarSetup {
            tyre_compound: 1,
            ..Default::default()
        };
        assert!(!softs.changes_car(), "nor does the next set's compound");
        assert_eq!(
            CarSetup::default().compound_index(&TireConfig::default()),
            crate::tyre_thermal::MEDIUM
        );
        let tuned = fuel_only.apply(&car());
        assert_eq!(
            tuned.fuel.capacity_liters,
            car().fuel.capacity_liters,
            "the tank is the file's"
        );
        assert!(CarSetup {
            spring_rear: 1,
            fuel_load: 3,
            ..Default::default()
        }
        .changes_car());
        assert_eq!(KNOBS[14].name, "fuel_load");
    }

    #[test]
    fn wings_trade_downforce_for_drag_and_ride_height_moves_the_map() {
        let mut base = car();
        base.aero.ride_height_sensitivity = 0.03;
        crate::aero::fit_reference(&mut base);
        let setup = CarSetup {
            front_wing: 2,
            rear_wing: -1,
            ride_height_front: -3,
            ..Default::default()
        };
        let t = setup.apply(&base);
        assert!((t.lift_coefficient_front - base.lift_coefficient_front * 1.10).abs() < 1e-6);
        assert!((t.lift_coefficient_rear - base.lift_coefficient_rear * 0.95).abs() < 1e-6);
        let drag = 1.01 * 0.985;
        assert!((t.drag_coefficient - base.drag_coefficient * drag).abs() < 1e-6);
        assert!(
            (t.aero.ride_height_front_m - (base.aero.ride_height_front_m - 0.006)).abs() < 1e-6
        );
        assert_eq!(t.aero.ride_height_rear_m, base.aero.ride_height_rear_m);
        assert_eq!(
            t.aero.reference_front_m, base.aero.reference_front_m,
            "the map's zero is the file's"
        );
        // The floor never goes through the road.
        let floor = CarSetup {
            ride_height_front: -5,
            ..Default::default()
        };
        let mut low = base.clone();
        low.aero.ride_height_front_m = 0.012;
        assert!(floor.apply(&low).aero.ride_height_front_m >= MIN_RIDE_HEIGHT_M);
    }

    #[test]
    fn geometry_knobs_move_the_static_camber_and_toe_not_the_filed_one() {
        let mut base = car();
        base.suspension.camber_front_deg = -3.0;
        base.suspension.camber_filed_front_deg = -3.0;
        base.suspension.toe_rear_deg = 0.15;
        let t = CarSetup {
            camber_front: 2,
            toe_rear: -1,
            ..Default::default()
        }
        .apply(&base);
        assert!(
            (t.suspension.camber_front_deg - -3.5).abs() < 1e-6,
            "more negative"
        );
        assert_eq!(t.suspension.camber_filed_front_deg, -3.0);
        assert!((t.suspension.toe_rear_deg - 0.10).abs() < 1e-6);
        assert!(CarSetup {
            toe_front: 1,
            ..Default::default()
        }
        .changes_car());
    }

    #[test]
    fn the_cooling_knobs_scale_their_inlets_and_cost_drag() {
        let base = car();
        let t = CarSetup {
            brake_ducts: 2,
            brake_ducts_rear: -3,
            radiator: 5,
            brake_pads: -1,
            ..Default::default()
        }
        .apply(&base);
        assert!((t.brake_duct_scale - 1.2).abs() < 1e-6);
        assert!((t.brake_duct_scale_rear - 0.7).abs() < 1e-6);
        assert!((t.engine.radiator_scale - base.engine.radiator_scale * 1.4).abs() < 1e-6);
        assert_eq!(t.brake_pads, crate::brakes::BrakePads::Endurance);
        let drag = (1.0 + 2.0 * BRAKE_DUCT_DRAG_PER_CLICK)
            * (1.0 - 3.0 * BRAKE_DUCT_DRAG_PER_CLICK)
            * (1.0 + 5.0 * RADIATOR_DRAG_PER_CLICK);
        assert!((t.drag_coefficient - base.drag_coefficient * drag).abs() < 1e-6);
        assert_eq!(KNOBS[25].name, "brake_ducts_rear");
        assert_eq!(KNOBS[26].name, "brake_pads");
        assert_eq!(KNOBS[27].name, "radiator");
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

    #[test]
    fn the_stock_compound_is_the_weathers_and_a_pick_is_honoured() {
        let tire = TireConfig::default();
        let stock = CarSetup::default();
        assert_eq!(
            stock.compound_index_for(&tire, 0.0),
            crate::tyre_thermal::MEDIUM
        );
        assert_eq!(
            stock.compound_index_for(&tire, 0.5),
            crate::tyre_thermal::INTERMEDIATE
        );
        assert_eq!(
            stock.compound_index_for(&tire, 1.0),
            crate::tyre_thermal::WET
        );
        let wets = CarSetup {
            tyre_compound: -3,
            ..CarSetup::default()
        }
        .clamp();
        assert_eq!(
            wets.compound_index_for(&tire, 0.0),
            crate::tyre_thermal::WET
        );
        let softs = CarSetup {
            tyre_compound: 1,
            ..CarSetup::default()
        };
        assert_eq!(
            softs.compound_index_for(&tire, 1.0),
            0,
            "slicks in the rain, if asked"
        );
        // A car with its own list: the knob is held to it.
        let own = TireConfig {
            compounds: crate::tyre_thermal::default_compounds()[..3].to_vec(),
            ..TireConfig::default()
        };
        assert_eq!(
            wets.compound_index_for(&own, 0.0),
            2,
            "hards: there is no wet"
        );
        assert_eq!(
            stock.compound_index_for(&own, 1.0),
            crate::tyre_thermal::MEDIUM,
            "slicks in the rain: the car has nothing else"
        );
    }
}
