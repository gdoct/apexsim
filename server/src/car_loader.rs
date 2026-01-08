use crate::data::*;
use serde::Deserialize;
use std::path::Path;
use uuid::Uuid;

#[derive(Debug, Deserialize)]
struct CarToml {
    id: String,
    name: String,
    #[allow(dead_code)]
    version: String,
    model: String,
    #[allow(dead_code)]
    texture_folder: Option<String>,
    physics: PhysicsToml,

    #[serde(default)]
    engine: Option<EngineToml>,
    #[serde(default)]
    transmission: Option<TransmissionToml>,
    #[serde(default)]
    drivetrain: Option<DrivetrainToml>,
    #[serde(default)]
    differential: Option<DifferentialToml>,
    #[serde(default)]
    fuel: Option<FuelToml>,
    #[serde(default)]
    hybrid: Option<HybridToml>,
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

#[derive(Debug, Deserialize, Default)]
struct TorqueCurvePointToml {
    rpm: f32,
    torque_nm: f32,
}

#[derive(Debug, Deserialize, Default)]
struct EngineToml {
    #[serde(default)]
    max_power_w: Option<f32>,
    #[serde(default)]
    max_torque_nm: Option<f32>,
    #[serde(default)]
    idle_rpm: Option<f32>,
    #[serde(default)]
    redline_rpm: Option<f32>,
    #[serde(default)]
    max_rpm: Option<f32>,
    #[serde(default)]
    rev_limiter_rpm: Option<f32>,

    #[serde(default)]
    inertia_kg_m2: Option<f32>,
    #[serde(default)]
    friction_torque_nm: Option<f32>,
    #[serde(default)]
    engine_brake_torque_nm: Option<f32>,
    #[serde(default)]
    idle_control_gain: Option<f32>,

    #[serde(default)]
    torque_curve: Vec<TorqueCurvePointToml>,
}

#[derive(Debug, Deserialize, Default)]
struct TransmissionToml {
    #[serde(default)]
    transmission_type: Option<String>,
    #[serde(default)]
    gear_ratios: Option<Vec<f32>>,
    #[serde(default)]
    final_drive_ratio: Option<f32>,
    #[serde(default)]
    shift_time_s: Option<f32>,
    #[serde(default)]
    efficiency: Option<f32>,
}

#[derive(Debug, Deserialize, Default)]
struct DrivetrainToml {
    #[serde(default)]
    layout: Option<String>,
}

#[derive(Debug, Deserialize, Default)]
struct DifferentialToml {
    #[serde(default)]
    differential_type: Option<String>,
    #[serde(default)]
    preload_nm: Option<f32>,
    #[serde(default)]
    lock_power: Option<f32>,
    #[serde(default)]
    lock_coast: Option<f32>,
}

#[derive(Debug, Deserialize, Default)]
struct FuelToml {
    #[serde(default)]
    capacity_liters: Option<f32>,
    #[serde(default)]
    idle_consumption_lps: Option<f32>,
    #[serde(default)]
    load_consumption_scale: Option<f32>,
}

#[derive(Debug, Deserialize, Default)]
struct HybridToml {
    #[serde(default)]
    enabled: Option<bool>,
    #[serde(default)]
    battery_capacity_kwh: Option<f32>,
    #[serde(default)]
    battery_max_discharge_kw: Option<f32>,
    #[serde(default)]
    battery_max_charge_kw: Option<f32>,
    #[serde(default)]
    motor_max_torque_nm: Option<f32>,
    #[serde(default)]
    motor_max_power_kw: Option<f32>,
    #[serde(default)]
    regen_max_power_kw: Option<f32>,
}

pub struct CarLoader;

impl CarLoader {
    pub fn load_from_file(path: &Path) -> Result<CarConfig, Box<dyn std::error::Error>> {
        let content = std::fs::read_to_string(path)?;
        let car_toml: CarToml = toml::from_str(&content)?;

        // Parse UUID from the ID string
        let id = Uuid::parse_str(&car_toml.id)?;

        let engine_toml = car_toml.engine.unwrap_or_default();
        let transmission_toml = car_toml.transmission.unwrap_or_default();
        let drivetrain_toml = car_toml.drivetrain.unwrap_or_default();
        let differential_toml = car_toml.differential.unwrap_or_default();
        let fuel_toml = car_toml.fuel.unwrap_or_default();
        let hybrid_toml = car_toml.hybrid.unwrap_or_default();

        // Convert engine force to power (legacy approximation: P = F * v, assuming ~100 m/s)
        let max_engine_power_w = engine_toml
            .max_power_w
            .unwrap_or_else(|| car_toml.physics.max_engine_force_n * 100.0);

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
            max_engine_torque_nm: engine_toml.max_torque_nm.unwrap_or(450.0),
            max_engine_rpm: engine_toml.max_rpm.unwrap_or(8000.0),
            idle_rpm: engine_toml.idle_rpm.unwrap_or(900.0),
            redline_rpm: engine_toml.redline_rpm.unwrap_or(7500.0),
            gear_ratios: transmission_toml
                .gear_ratios
                .unwrap_or_else(|| vec![-3.5, 3.8, 2.4, 1.7, 1.3, 1.0, 0.8]),
            final_drive_ratio: transmission_toml.final_drive_ratio.unwrap_or(3.7),
            drivetrain: match drivetrain_toml.layout.as_deref() {
                Some("FWD") | Some("fwd") => Drivetrain::FWD,
                Some("AWD") | Some("awd") | Some("4WD") | Some("4wd") => Drivetrain::AWD,
                _ => Drivetrain::RWD,
            },

            engine: EngineConfig {
                rev_limiter_rpm: engine_toml.rev_limiter_rpm.unwrap_or_else(|| engine_toml.max_rpm.unwrap_or(8000.0)),
                torque_curve: engine_toml
                    .torque_curve
                    .into_iter()
                    .map(|p| TorqueCurvePoint {
                        rpm: p.rpm,
                        torque_nm: p.torque_nm,
                    })
                    .collect(),
                inertia_kg_m2: engine_toml.inertia_kg_m2.unwrap_or(0.25),
                friction_torque_nm: engine_toml.friction_torque_nm.unwrap_or(20.0),
                engine_brake_torque_nm: engine_toml.engine_brake_torque_nm.unwrap_or(80.0),
                idle_control_gain: engine_toml.idle_control_gain.unwrap_or(0.15),
            },
            transmission: TransmissionConfig {
                transmission_type: match transmission_toml.transmission_type.as_deref() {
                    Some("Manual") | Some("manual") => TransmissionType::Manual,
                    Some("DCT") | Some("dct") => TransmissionType::DCT,
                    Some("Automatic") | Some("automatic") => TransmissionType::Automatic,
                    Some("CVT") | Some("cvt") => TransmissionType::CVT,
                    _ => TransmissionType::Sequential,
                },
                shift_time_s: transmission_toml.shift_time_s.unwrap_or(0.12),
                efficiency: transmission_toml.efficiency.unwrap_or(0.92),
            },
            differential: DifferentialConfig {
                differential_type: match differential_toml.differential_type.as_deref() {
                    Some("Open") | Some("open") => DifferentialType::Open,
                    Some("Locked") | Some("locked") | Some("Spool") | Some("spool") => DifferentialType::Locked,
                    Some("ViscousLSD") | Some("viscous") | Some("viscous_lsd") => DifferentialType::ViscousLSD,
                    Some("Torsen") | Some("torsen") => DifferentialType::Torsen,
                    _ => DifferentialType::ClutchLSD,
                },
                preload_nm: differential_toml.preload_nm.unwrap_or(60.0),
                lock_power: differential_toml.lock_power.unwrap_or(0.35),
                lock_coast: differential_toml.lock_coast.unwrap_or(0.20),
            },
            fuel: FuelConfig {
                capacity_liters: fuel_toml.capacity_liters.unwrap_or(100.0),
                idle_consumption_lps: fuel_toml.idle_consumption_lps.unwrap_or(0.00005),
                load_consumption_scale: fuel_toml.load_consumption_scale.unwrap_or(0.003),
            },
            hybrid: HybridConfig {
                enabled: hybrid_toml.enabled.unwrap_or(false),
                battery_capacity_kwh: hybrid_toml.battery_capacity_kwh.unwrap_or(0.0),
                battery_max_discharge_kw: hybrid_toml.battery_max_discharge_kw.unwrap_or(0.0),
                battery_max_charge_kw: hybrid_toml.battery_max_charge_kw.unwrap_or(0.0),
                motor_max_torque_nm: hybrid_toml.motor_max_torque_nm.unwrap_or(0.0),
                motor_max_power_kw: hybrid_toml.motor_max_power_kw.unwrap_or(0.0),
                regen_max_power_kw: hybrid_toml.regen_max_power_kw.unwrap_or(0.0),
            },

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
