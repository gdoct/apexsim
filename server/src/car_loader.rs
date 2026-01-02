use crate::data::*;
use serde::Deserialize;
use std::path::Path;
use uuid::Uuid;

#[derive(Debug, Deserialize)]
struct CarToml {
    id: String,
    name: String,
    version: String,
    model: String,
    texture_folder: Option<String>,
    physics: PhysicsToml,
}

#[derive(Debug, Deserialize)]
struct PhysicsToml {
    mass_kg: f32,
    #[serde(default)]
    length_m: Option<f32>,
    #[serde(default)]
    width_m: Option<f32>,
    max_engine_force_n: f32,
    max_brake_force_n: f32,
    drag_coefficient: f32,
    grip_coefficient: f32,
    max_steering_angle_rad: f32,
    wheelbase_m: f32,
}

pub struct CarLoader;

impl CarLoader {
    pub fn load_from_file(path: &Path) -> Result<CarConfig, Box<dyn std::error::Error>> {
        let content = std::fs::read_to_string(path)?;
        let car_toml: CarToml = toml::from_str(&content)?;

        // Parse UUID from the ID string
        let id = Uuid::parse_str(&car_toml.id)?;

        // Convert engine force to power (rough approximation: P = F * v, assuming ~100 m/s)
        let max_engine_power_w = car_toml.physics.max_engine_force_n * 100.0;

        println!("  Loaded {}: mass={}kg, engine_force={}N, power={}W",
            car_toml.name, car_toml.physics.mass_kg, car_toml.physics.max_engine_force_n, max_engine_power_w);

        Ok(CarConfig {
            id,
            name: car_toml.name,
            model: car_toml.model,

            // Physical dimensions
            mass_kg: car_toml.physics.mass_kg,
            length_m: car_toml.physics.length_m.unwrap_or(4.5),
            width_m: car_toml.physics.width_m.unwrap_or(1.9),
            height_m: 1.3,
            wheelbase_m: car_toml.physics.wheelbase_m,
            track_width_front_m: 1.6,
            track_width_rear_m: 1.58,
            wheel_radius_m: 0.33,

            // Center of gravity
            cog_height_m: 0.45,
            cog_offset_x_m: 0.0,
            weight_distribution_front: 0.52,

            // Engine & drivetrain
            max_engine_power_w,
            max_engine_torque_nm: 450.0,
            max_engine_rpm: 8000.0,
            idle_rpm: 900.0,
            redline_rpm: 7500.0,
            gear_ratios: vec![-3.5, 3.8, 2.4, 1.7, 1.3, 1.0, 0.8],
            final_drive_ratio: 3.7,
            drivetrain: Drivetrain::RWD,

            // Braking
            max_brake_force_n: car_toml.physics.max_brake_force_n,
            brake_bias_front: 0.6,
            abs_enabled: true,

            // Aerodynamics
            drag_coefficient: car_toml.physics.drag_coefficient,
            frontal_area_m2: 2.2,
            lift_coefficient_front: -0.15,
            lift_coefficient_rear: -0.20,

            // Steering
            max_steering_angle_rad: car_toml.physics.max_steering_angle_rad,
            steering_ratio: 14.0,

            // Suspension
            suspension: SuspensionConfig::default(),

            // Tires
            tire_config: TireConfig {
                grip_coefficient: car_toml.physics.grip_coefficient,
                ..TireConfig::default()
            },
        })
    }
}
