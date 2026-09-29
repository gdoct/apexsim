//! Every car `scripts/ac_car_import.py` has written into
//! `content/cars/custom` (its car.toml says `imported = "ac"`) loads the
//! way the server will load it, and its figures are in the range of a real
//! car. Without an import on this machine there is nothing to check and
//! the test says so and passes.

use std::path::{Path, PathBuf};

use apexsim_server::car_loader::{car_toml_paths, drag_limited_speed_mps, CarLoader};

fn imported_car_tomls() -> Vec<PathBuf> {
    let dir = Path::new(env!("CARGO_MANIFEST_DIR")).join("../content/cars/custom");
    car_toml_paths(&dir)
        .into_iter()
        .filter(|p| std::fs::read_to_string(p).is_ok_and(|t| t.contains("imported = \"ac\"")))
        .collect()
}

// Opt-in: the imports on a given machine are the player's own data, and a
// bad one should not fail the suite. Run it after importing a car with
// `cargo test --release --test imported_car_test -- --ignored --nocapture`.
#[test]
#[ignore]
fn every_imported_car_loads_with_plausible_figures() {
    let paths = imported_car_tomls();
    if paths.is_empty() {
        eprintln!("no imported cars under content/cars/custom; nothing to check");
        return;
    }
    for path in paths {
        let folder = path
            .parent()
            .unwrap()
            .file_name()
            .unwrap()
            .to_string_lossy()
            .into_owned();
        let car = CarLoader::load_from_file(&path)
            .unwrap_or_else(|e| panic!("{folder}: the server cannot load it: {e}"));
        let top_speed_kmh = drag_limited_speed_mps(&car) * 3.6;
        let cla = -(car.lift_coefficient_front + car.lift_coefficient_rear) * car.frontal_area_m2;
        println!(
            "{folder}: {} [{}] {:.0} kg, {:.0} kW, {} gears, grip {:.3}, Cd.A {:.2} Cl.A {:.2}, \
             top speed {top_speed_kmh:.0} km/h, {} liveries",
            car.name,
            car.class,
            car.mass_kg,
            car.max_engine_power_w / 1000.0,
            car.gear_ratios.len() - 1,
            car.tire_config.grip_coefficient,
            car.drag_coefficient * car.frontal_area_m2,
            cla,
            car.livery_names.len(),
        );
        assert!(
            (300.0..3000.0).contains(&car.mass_kg),
            "{folder}: mass {}",
            car.mass_kg
        );
        assert!(
            (0.5..2.5).contains(&car.tire_config.grip_coefficient),
            "{folder}: grip {}",
            car.tire_config.grip_coefficient
        );
        assert!(
            (20_000.0..2_000_000.0).contains(&car.max_engine_power_w),
            "{folder}: power {} W",
            car.max_engine_power_w
        );
        assert!(
            car.engine.torque_curve.len() >= 2,
            "{folder}: no torque curve"
        );
        assert!(
            (80.0..450.0).contains(&top_speed_kmh),
            "{folder}: drag-limited top speed {top_speed_kmh:.0} km/h"
        );
        assert!(cla > -1.0, "{folder}: Cl.A {cla:.2} is a car that flies");
    }
}
