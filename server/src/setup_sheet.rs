//! The garage's reference card: what each setup knob is in real units.
//!
//! A setup travels as clicks (`crate::car_setup`), so the client never needs
//! the car's figures to tune it — but a garage that says "+2 (+8%)" cannot
//! tell a driver that the front springs are 120 N/mm, the front ride height
//! 58 mm or that sixth tops out at 284 km/h. Many of those figures are not in
//! the car.toml at all but the sim's defaults, so only the server can say.
//!
//! [`CarSetupSheetData`] is sent once, right after `SessionJoined`: for every
//! knob in `KNOBS` order its stock value, the value of one click and the
//! bounds `apply` holds it to, all in display units; plus what the garage
//! derives from the setup on its own (gear ratios, wheel radius, fuel per
//! lap). Every knob is linear in its clicks, so the client works out any
//! setup's figures as `stock + clicks * step`, clamped, with no round trip.
//! The sheet only describes: nothing here changes how a setup is applied.

use crate::car_setup::{self, KNOBS, KNOB_COUNT};
use crate::data::{CarConfig, CarConfigId, SessionId};
use serde::{Deserialize, Serialize};

/// kPa in a psi: pressures are shown the way a garage reads them.
const KPA_PER_PSI: f32 = 6.894_757;
/// Air density and speed the wing figures are quoted at: downforce in kg
/// at 200 km/h on a standard day, per axle.
const WING_QUOTE_RHO: f32 = 1.225;
const WING_QUOTE_MPS: f32 = 200.0 / 3.6;
const G: f32 = 9.81;

/// One knob in real units: the value at `stock + clicks * step`, held to
/// `lo..=hi`, printed with `decimals` and `unit`.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct SetupKnobFigure {
    pub stock: f32,
    pub step: f32,
    pub lo: f32,
    pub hi: f32,
    pub decimals: u8,
    pub unit: String,
}

/// The garage's reference card for one car on this session's track.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "PascalCase")]
pub struct CarSetupSheetData {
    #[serde(
        serialize_with = "crate::network::serialize_uuid_as_string",
        deserialize_with = "crate::network::deserialize_uuid_from_string"
    )]
    pub session_id: SessionId,
    #[serde(
        serialize_with = "crate::network::serialize_uuid_as_string",
        deserialize_with = "crate::network::deserialize_uuid_from_string"
    )]
    pub car_config_id: CarConfigId,
    /// One per knob, in `KNOBS` order (the tyre compound is 0 / 1 and read
    /// out by name).
    pub knobs: Vec<SetupKnobFigure>,
    /// Forward gears, first first, and the final drive, as filed: the
    /// gearing knobs scale them as `car_setup::CarSetup::apply` does.
    pub gear_ratios: Vec<f32>,
    pub final_drive: f32,
    pub wheel_radius_m: f32,
    /// The hot pressure the tyres grip best at, psi.
    pub tyre_optimal_psi: f32,
    /// Litres a lap of this track costs the car (0 when there is no line
    /// to plan on), the fuel's weight and the laps a hotlap fills.
    pub lap_fuel_l: f32,
    pub fuel_kg_per_l: f32,
    pub fill_laps: f32,
    /// Whether the car.toml gave camber, without which camber changes nothing.
    pub camber_modelled: bool,
    /// How far the front's share of the downforce moves per millimetre of
    /// rake (rear height less front) over the stock rake (`crate::aero`).
    pub rake_balance_per_mm: f32,
}

/// Downforce in kg at the quote speed for a lift coefficient.
fn downforce_kg(car: &CarConfig, lift: f32) -> f32 {
    0.5 * WING_QUOTE_RHO * WING_QUOTE_MPS * WING_QUOTE_MPS * car.frontal_area_m2 * lift.abs() / G
}

fn figure(stock: f32, step: f32, decimals: u8, unit: &str) -> SetupKnobFigure {
    SetupKnobFigure {
        stock,
        step,
        lo: f32::MIN,
        hi: f32::MAX,
        decimals,
        unit: unit.to_string(),
    }
}

/// The figure for one knob of `car`, by the knob's wire name.
fn knob_figure(name: &str, car: &CarConfig, lap_fuel_l: f32, fill_laps: f32) -> SetupKnobFigure {
    let s = &car.suspension;
    let percent_of = |base: f32, per_click: f32| base * per_click;
    match name {
        "tyre_pressure_front" => figure(
            car.tire_config.pressure_front_kpa / KPA_PER_PSI,
            car_setup::TYRE_PRESSURE_KPA_PER_CLICK / KPA_PER_PSI,
            1,
            "psi",
        ),
        "tyre_pressure_rear" => figure(
            car.tire_config.pressure_rear_kpa / KPA_PER_PSI,
            car_setup::TYRE_PRESSURE_KPA_PER_CLICK / KPA_PER_PSI,
            1,
            "psi",
        ),
        "rev_limiter" => SetupKnobFigure {
            lo: car.idle_rpm + 500.0,
            ..figure(
                car.redline_rpm,
                car_setup::REV_LIMITER_RPM_PER_CLICK,
                0,
                "rpm",
            )
        },
        "engine_braking" => {
            let nm = car.engine.engine_brake_torque_nm;
            figure(
                nm,
                percent_of(nm, car_setup::ENGINE_BRAKING_PER_CLICK),
                0,
                "Nm",
            )
        }
        "final_drive" => figure(
            car.final_drive_ratio,
            percent_of(car.final_drive_ratio, car_setup::FINAL_DRIVE_PER_CLICK),
            3,
            "",
        ),
        "gear_spread" => {
            let top = car.gear_ratios.last().copied().unwrap_or(1.0);
            figure(
                top,
                percent_of(top, car_setup::GEAR_SPREAD_PER_CLICK),
                3,
                "top",
            )
        }
        "torque_map" => figure(100.0, car_setup::TORQUE_MAP_PER_CLICK * 100.0, 0, "%"),
        "brake_bias" => SetupKnobFigure {
            lo: 5.0,
            hi: 95.0,
            ..figure(
                car.brake_bias_front * 100.0,
                car_setup::BRAKE_BIAS_PER_CLICK * 100.0,
                1,
                "% F",
            )
        },
        "spring_front" => {
            let n_mm = s.spring_rate_front_n_per_m / 1000.0;
            figure(
                n_mm,
                percent_of(n_mm, car_setup::SPRING_PER_CLICK),
                1,
                "N/mm",
            )
        }
        "spring_rear" => {
            let n_mm = s.spring_rate_rear_n_per_m / 1000.0;
            figure(
                n_mm,
                percent_of(n_mm, car_setup::SPRING_PER_CLICK),
                1,
                "N/mm",
            )
        }
        "damper_front" => {
            let b = s.damper_compression_front;
            figure(b, percent_of(b, car_setup::DAMPER_PER_CLICK), 0, "Ns/m")
        }
        "damper_rear" => {
            let b = s.damper_compression_rear;
            figure(b, percent_of(b, car_setup::DAMPER_PER_CLICK), 0, "Ns/m")
        }
        "anti_roll_front" => {
            let k = s.anti_roll_bar_front / 1000.0;
            figure(k, percent_of(k, car_setup::ANTI_ROLL_PER_CLICK), 1, "N/mm")
        }
        "anti_roll_rear" => {
            let k = s.anti_roll_bar_rear / 1000.0;
            figure(k, percent_of(k, car_setup::ANTI_ROLL_PER_CLICK), 1, "N/mm")
        }
        "fuel_load" => {
            let tank = car.fuel.capacity_liters;
            if lap_fuel_l > 0.0 {
                SetupKnobFigure {
                    lo: (crate::game_session::MIN_FUEL_LAPS * lap_fuel_l).min(tank),
                    hi: tank,
                    ..figure(
                        (fill_laps * lap_fuel_l).min(tank),
                        car_setup::FUEL_LAPS_PER_CLICK * lap_fuel_l,
                        1,
                        "L",
                    )
                }
            } else {
                // No line to plan a lap's fuel on: the knob in laps.
                figure(fill_laps, car_setup::FUEL_LAPS_PER_CLICK, 0, "laps")
            }
        }
        "front_wing" => {
            let kg = downforce_kg(car, car.lift_coefficient_front);
            figure(kg, percent_of(kg, car_setup::WING_PER_CLICK), 0, "kg")
        }
        "rear_wing" => {
            let kg = downforce_kg(car, car.lift_coefficient_rear);
            figure(kg, percent_of(kg, car_setup::WING_PER_CLICK), 0, "kg")
        }
        "ride_height_front" => SetupKnobFigure {
            lo: 10.0,
            ..figure(
                car.aero.ride_height_front_m * 1000.0,
                car_setup::RIDE_HEIGHT_M_PER_CLICK * 1000.0,
                0,
                "mm",
            )
        },
        "ride_height_rear" => SetupKnobFigure {
            lo: 10.0,
            ..figure(
                car.aero.ride_height_rear_m * 1000.0,
                car_setup::RIDE_HEIGHT_M_PER_CLICK * 1000.0,
                0,
                "mm",
            )
        },
        "tyre_compound" => figure(0.0, 1.0, 0, ""),
        "brake_ducts" => figure(100.0, car_setup::BRAKE_DUCT_PER_CLICK * 100.0, 0, "%"),
        "camber_front" => SetupKnobFigure {
            lo: -8.0,
            hi: 3.0,
            ..figure(s.camber_front_deg, car_setup::CAMBER_DEG_PER_CLICK, 2, "°")
        },
        "camber_rear" => SetupKnobFigure {
            lo: -8.0,
            hi: 3.0,
            ..figure(s.camber_rear_deg, car_setup::CAMBER_DEG_PER_CLICK, 2, "°")
        },
        "toe_front" => figure(s.toe_front_deg, car_setup::TOE_DEG_PER_CLICK, 2, "°"),
        "toe_rear" => figure(s.toe_rear_deg, car_setup::TOE_DEG_PER_CLICK, 2, "°"),
        // A knob this table has not been taught yet reads as its clicks.
        _ => figure(0.0, 1.0, 0, ""),
    }
}

/// The sheet for `car`, whose lap costs `lap_fuel_l` litres (0 when unknown)
/// and whose hotlap fill is `fill_laps` laps.
pub fn build(
    session_id: SessionId,
    car: &CarConfig,
    lap_fuel_l: f32,
    fill_laps: f32,
) -> CarSetupSheetData {
    let knobs = KNOBS
        .iter()
        .map(|knob| knob_figure(knob.name, car, lap_fuel_l, fill_laps))
        .collect::<Vec<_>>();
    debug_assert_eq!(knobs.len(), KNOB_COUNT);
    CarSetupSheetData {
        session_id,
        car_config_id: car.id,
        knobs,
        // Index 0 is reverse.
        gear_ratios: car.gear_ratios.iter().skip(1).copied().collect(),
        final_drive: car.final_drive_ratio,
        wheel_radius_m: car.wheel_radius_m,
        tyre_optimal_psi: car.tire_config.optimal_pressure_kpa / KPA_PER_PSI,
        lap_fuel_l: lap_fuel_l.max(0.0),
        fuel_kg_per_l: car.fuel.density_kg_per_l,
        fill_laps,
        camber_modelled: car.suspension.camber_modelled,
        rake_balance_per_mm: car.aero.rake_sensitivity / 10.0,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::car_setup::CarSetup;

    /// What the client works out from the sheet for a knob at `clicks`.
    fn value(f: &SetupKnobFigure, clicks: i8) -> f32 {
        (f.stock + clicks as f32 * f.step).clamp(f.lo, f.hi)
    }

    #[test]
    fn every_knob_has_a_figure_and_stock_is_the_file() {
        let car = CarConfig::default();
        let sheet = build(SessionId::nil(), &car, 3.0, 3.0);
        assert_eq!(sheet.knobs.len(), KNOB_COUNT);
        let spring = &sheet.knobs[8];
        assert!((spring.stock - car.suspension.spring_rate_front_n_per_m / 1000.0).abs() < 1e-3);
        assert_eq!(sheet.gear_ratios.len(), car.gear_ratios.len() - 1);
        for f in &sheet.knobs {
            assert!(f.stock.is_finite() && f.step.is_finite(), "{f:?}");
        }
    }

    /// The sheet's linear read-out agrees with what `apply` bakes into the car.
    #[test]
    fn the_sheet_predicts_what_apply_does() {
        let car = CarConfig::default();
        let sheet = build(SessionId::nil(), &car, 3.0, 3.0);
        let setup = CarSetup {
            spring_rear: 3,
            ride_height_front: -2,
            brake_bias: 2,
            final_drive: -4,
            rev_limiter: -3,
            ..Default::default()
        };
        let t = setup.apply(&car);
        let close = |a: f32, b: f32| (a - b).abs() < 1e-3 * b.abs().max(1.0);
        assert!(close(
            value(&sheet.knobs[9], 3),
            t.suspension.spring_rate_rear_n_per_m / 1000.0
        ));
        assert!(close(
            value(&sheet.knobs[17], -2),
            t.aero.ride_height_front_m * 1000.0
        ));
        assert!(close(value(&sheet.knobs[7], 2), t.brake_bias_front * 100.0));
        assert!(close(value(&sheet.knobs[4], -4), t.final_drive_ratio));
        assert!(close(value(&sheet.knobs[2], -3), t.redline_rpm));
    }

    #[test]
    fn fuel_reads_in_litres_when_a_lap_is_known_and_laps_otherwise() {
        let car = CarConfig::default();
        let known = build(SessionId::nil(), &car, 2.5, 3.0);
        assert_eq!(known.knobs[14].unit, "L");
        assert!((known.knobs[14].stock - 7.5).abs() < 1e-4);
        assert!((known.knobs[14].step - 2.5).abs() < 1e-4);
        let unknown = build(SessionId::nil(), &car, 0.0, 3.0);
        assert_eq!(unknown.knobs[14].unit, "laps");
    }
}
