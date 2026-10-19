//! Balance of performance: a server's per-car ballast, power and downforce.
//!
//! A league evens out a mixed grid by the car, not the driver: `[[balance]]`
//! tables in server.toml name a car (its `car.toml` `name` or id) and what
//! it carries. They are applied once, to the cars as loaded, so every
//! session, the AI's plans and the racing line see the balanced car, and a
//! driver's setup works from it (the torque map scales the balanced curve).
//! A car no table names is the car as loaded, bit for bit.

use serde::{Deserialize, Serialize};

use crate::data::CarConfig;

/// The bounds a table is held to, so a typo cannot make a car undriveable.
pub const BALLAST_KG: (f32, f32) = (-200.0, 300.0);
pub const POWER: (f32, f32) = (0.5, 1.5);
pub const DOWNFORCE: (f32, f32) = (0.5, 1.5);

/// One car's balance (`[[balance]]`).
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
#[serde(default)]
pub struct CarBalance {
    /// The car: its `car.toml` `name` (case ignored) or its id.
    pub car: String,
    /// Mass added at the car's centre of gravity, kg; negative takes off.
    pub ballast_kg: f32,
    /// The engine's torque, and so its power, against the car's own: the
    /// whole curve scaled, as the setup's torque map does.
    pub power: f32,
    /// The downforce (both axles' lift coefficients) against the car's own;
    /// drag is left alone.
    pub downforce: f32,
}

impl Default for CarBalance {
    fn default() -> Self {
        Self {
            car: String::new(),
            ballast_kg: 0.0,
            power: 1.0,
            downforce: 1.0,
        }
    }
}

impl CarBalance {
    /// Whether this table names `car`.
    pub fn names(&self, car: &CarConfig) -> bool {
        let wanted = self.car.trim();
        wanted.eq_ignore_ascii_case(car.name.trim()) || wanted == car.id.to_string()
    }

    /// What is wrong with the table, if anything.
    pub fn problem(&self) -> Option<String> {
        let out = |v: f32, (lo, hi): (f32, f32)| !(v.is_finite() && (lo..=hi).contains(&v));
        if self.car.trim().is_empty() {
            Some("a [[balance]] table names no car".into())
        } else if out(self.ballast_kg, BALLAST_KG) {
            Some(format!(
                "balance for {}: ballast_kg must be in {:?}",
                self.car, BALLAST_KG
            ))
        } else if out(self.power, POWER) {
            Some(format!(
                "balance for {}: power must be in {:?}",
                self.car, POWER
            ))
        } else if out(self.downforce, DOWNFORCE) {
            Some(format!(
                "balance for {}: downforce must be in {:?}",
                self.car, DOWNFORCE
            ))
        } else {
            None
        }
    }

    /// Apply to `car`. Only what the table changes is touched.
    pub fn apply(&self, car: &mut CarConfig) {
        if self.ballast_kg != 0.0 {
            car.mass_kg = (car.mass_kg + self.ballast_kg).max(1.0);
        }
        if self.power != 1.0 {
            for point in car.engine.torque_curve.iter_mut() {
                point.torque_nm *= self.power;
            }
            car.max_engine_torque_nm *= self.power;
            car.max_engine_power_w *= self.power;
        }
        if self.downforce != 1.0 {
            car.lift_coefficient_front *= self.downforce;
            car.lift_coefficient_rear *= self.downforce;
        }
    }
}

/// Apply every table to the car it names; the tables that named no car.
pub fn apply_all<'a, 'b>(
    tables: &'a [CarBalance],
    cars: impl IntoIterator<Item = &'b mut CarConfig>,
) -> Vec<&'a CarBalance> {
    let mut used = vec![false; tables.len()];
    for car in cars {
        // The first table naming a car wins: a car is balanced once.
        if let Some(i) = tables.iter().position(|t| t.names(car)) {
            tables[i].apply(car);
            used[i] = true;
        }
    }
    tables
        .iter()
        .zip(used)
        .filter(|(_, used)| !used)
        .map(|(t, _)| t)
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::data::TorqueCurvePoint;

    fn car(name: &str) -> CarConfig {
        let mut car = CarConfig {
            name: name.into(),
            id: uuid::Uuid::new_v4(),
            ..CarConfig::default()
        };
        car.engine.torque_curve = vec![
            TorqueCurvePoint {
                rpm: 3000.0,
                torque_nm: 400.0,
            },
            TorqueCurvePoint {
                rpm: 7000.0,
                torque_nm: 500.0,
            },
        ];
        car
    }

    #[test]
    fn a_balanced_car_carries_its_table_and_the_rest_are_untouched() {
        let tables = vec![
            CarBalance {
                car: "gt3 alpha".into(),
                ballast_kg: 30.0,
                power: 0.95,
                downforce: 1.1,
            },
            CarBalance {
                car: "Nobody".into(),
                ..Default::default()
            },
        ];
        let mut a = car("GT3 Alpha");
        let mut b = car("GT3 Beta");
        let (a0, b0) = (a.clone(), b.clone());
        let unused = apply_all(&tables, [&mut a, &mut b]);
        assert_eq!(unused.len(), 1);
        assert_eq!(unused[0].car, "Nobody");
        assert_eq!(a.mass_kg, a0.mass_kg + 30.0);
        assert_eq!(a.engine.torque_curve[1].torque_nm, 500.0 * 0.95);
        assert_eq!(a.max_engine_power_w, a0.max_engine_power_w * 0.95);
        assert_eq!(a.lift_coefficient_rear, a0.lift_coefficient_rear * 1.1);
        assert_eq!(a.drag_coefficient, a0.drag_coefficient);
        // Bit for bit: the config serialises the same.
        assert_eq!(
            serde_json::to_string(&b).unwrap(),
            serde_json::to_string(&b0).unwrap()
        );
    }

    #[test]
    fn a_car_is_named_by_its_id_too() {
        let mut a = car("Whatever");
        let tables = vec![CarBalance {
            car: a.id.to_string(),
            ballast_kg: 10.0,
            ..Default::default()
        }];
        let m = a.mass_kg;
        assert!(apply_all(&tables, [&mut a]).is_empty());
        assert_eq!(a.mass_kg, m + 10.0);
    }

    #[test]
    fn out_of_bounds_tables_are_refused() {
        let ok = CarBalance {
            car: "X".into(),
            ..Default::default()
        };
        assert!(ok.problem().is_none());
        for bad in [
            CarBalance {
                car: String::new(),
                ..ok.clone()
            },
            CarBalance {
                ballast_kg: 1000.0,
                ..ok.clone()
            },
            CarBalance {
                power: 3.0,
                ..ok.clone()
            },
            CarBalance {
                downforce: f32::NAN,
                ..ok.clone()
            },
        ] {
            assert!(bad.problem().is_some(), "{bad:?}");
        }
    }

    #[test]
    fn a_table_reads_from_toml() {
        #[derive(Deserialize)]
        struct T {
            balance: Vec<CarBalance>,
        }
        let t: T = toml::from_str(
            "[[balance]]\ncar = \"GT3 Alpha\"\nballast_kg = 15\n\n[[balance]]\ncar = \"B\"\npower = 0.97\n",
        )
        .unwrap();
        assert_eq!(t.balance[0].ballast_kg, 15.0);
        assert_eq!(t.balance[0].power, 1.0);
        assert_eq!(t.balance[1].power, 0.97);
    }
}
