use crate::data::*;
use serde::Deserialize;
use std::path::Path;
use tracing::debug;
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
    #[serde(default)]
    suspension: Option<SuspensionToml>,
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

    // Optional moddable parameters. Defaults (applied in `load_from_file`)
    // match the values previously hardcoded for every car.
    #[serde(default)]
    height_m: Option<f32>,
    #[serde(default)]
    track_width_front_m: Option<f32>,
    #[serde(default)]
    track_width_rear_m: Option<f32>,
    #[serde(default)]
    wheel_radius_m: Option<f32>,
    #[serde(default)]
    cog_height_m: Option<f32>,
    #[serde(default)]
    weight_distribution_front: Option<f32>,
    #[serde(default)]
    brake_bias_front: Option<f32>,
    #[serde(default)]
    abs_enabled: Option<bool>,
    #[serde(default)]
    traction_control_enabled: Option<bool>,
    #[serde(default)]
    frontal_area_m2: Option<f32>,
    #[serde(default)]
    lift_coefficient_front: Option<f32>,
    #[serde(default)]
    lift_coefficient_rear: Option<f32>,
    #[serde(default)]
    steering_ratio: Option<f32>,
}

/// Optional `[suspension]` section. Unset fields fall back to
/// `SuspensionConfig::default()`, so existing cars behave identically.
#[derive(Debug, Deserialize, Default)]
struct SuspensionToml {
    #[serde(default)]
    spring_rate_front_n_per_m: Option<f32>,
    #[serde(default)]
    spring_rate_rear_n_per_m: Option<f32>,
    #[serde(default)]
    damper_compression_front: Option<f32>,
    #[serde(default)]
    damper_compression_rear: Option<f32>,
    #[serde(default)]
    damper_rebound_front: Option<f32>,
    #[serde(default)]
    damper_rebound_rear: Option<f32>,
    #[serde(default)]
    anti_roll_bar_front: Option<f32>,
    #[serde(default)]
    anti_roll_bar_rear: Option<f32>,
    #[serde(default)]
    max_travel_m: Option<f32>,
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

#[derive(Debug, thiserror::Error)]
pub enum CarLoadError {
    #[error("failed to read {path}: {source}")]
    Io {
        path: String,
        source: std::io::Error,
    },
    #[error("failed to parse {path}: {source}")]
    Parse {
        path: String,
        source: Box<toml::de::Error>,
    },
    #[error("invalid car id in {path}: {source}")]
    InvalidId { path: String, source: uuid::Error },
    #[error("invalid car config {path}: {reason}")]
    Invalid { path: String, reason: String },
}

pub struct CarLoader;

impl CarLoader {
    pub fn load_from_file(path: &Path) -> Result<CarConfig, CarLoadError> {
        let path_str = path.display().to_string();
        let content = std::fs::read_to_string(path).map_err(|source| CarLoadError::Io {
            path: path_str.clone(),
            source,
        })?;
        let car_toml: CarToml = toml::from_str(&content).map_err(|source| CarLoadError::Parse {
            path: path_str.clone(),
            source: Box::new(source),
        })?;

        // Parse UUID from the ID string
        let id = Uuid::parse_str(&car_toml.id).map_err(|source| CarLoadError::InvalidId {
            path: path_str.clone(),
            source,
        })?;

        let engine_toml = car_toml.engine.unwrap_or_default();
        let transmission_toml = car_toml.transmission.unwrap_or_default();
        let drivetrain_toml = car_toml.drivetrain.unwrap_or_default();
        let differential_toml = car_toml.differential.unwrap_or_default();
        let fuel_toml = car_toml.fuel.unwrap_or_default();
        let hybrid_toml = car_toml.hybrid.unwrap_or_default();
        let suspension_toml = car_toml.suspension.unwrap_or_default();
        let suspension_defaults = SuspensionConfig::default();

        // Convert engine force to power (legacy approximation: P = F * v, assuming ~100 m/s)
        let max_engine_power_w = engine_toml
            .max_power_w
            .unwrap_or(car_toml.physics.max_engine_force_n * 100.0);

        debug!(
            "  Loaded {}: mass={}kg, engine_force={}N, power={}W",
            car_toml.name,
            car_toml.physics.mass_kg,
            car_toml.physics.max_engine_force_n,
            max_engine_power_w
        );

        let config = CarConfig {
            id,
            name: car_toml.name,
            model: car_toml.model,

            // Physical dimensions
            mass_kg: car_toml.physics.mass_kg,
            length_m: car_toml.physics.length_m.unwrap_or(4.5),
            width_m: car_toml.physics.width_m.unwrap_or(1.9),
            height_m: car_toml.physics.height_m.unwrap_or(1.3),
            wheelbase_m: car_toml.physics.wheelbase_m,
            track_width_front_m: car_toml.physics.track_width_front_m.unwrap_or(1.6),
            track_width_rear_m: car_toml.physics.track_width_rear_m.unwrap_or(1.58),
            wheel_radius_m: car_toml.physics.wheel_radius_m.unwrap_or(0.33),

            // Center of gravity
            cog_height_m: car_toml.physics.cog_height_m.unwrap_or(0.45),
            cog_offset_x_m: 0.0,
            weight_distribution_front: car_toml.physics.weight_distribution_front.unwrap_or(0.52),

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
                rev_limiter_rpm: engine_toml
                    .rev_limiter_rpm
                    .unwrap_or_else(|| engine_toml.max_rpm.unwrap_or(8000.0)),
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
                    Some("Locked") | Some("locked") | Some("Spool") | Some("spool") => {
                        DifferentialType::Locked
                    }
                    Some("ViscousLSD") | Some("viscous") | Some("viscous_lsd") => {
                        DifferentialType::ViscousLSD
                    }
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
            brake_bias_front: car_toml.physics.brake_bias_front.unwrap_or(0.6),
            abs_enabled: car_toml.physics.abs_enabled.unwrap_or(true),
            traction_control_enabled: car_toml.physics.traction_control_enabled.unwrap_or(true),

            // Aerodynamics
            drag_coefficient: car_toml.physics.drag_coefficient,
            frontal_area_m2: car_toml.physics.frontal_area_m2.unwrap_or(2.2),
            lift_coefficient_front: car_toml.physics.lift_coefficient_front.unwrap_or(-0.15),
            lift_coefficient_rear: car_toml.physics.lift_coefficient_rear.unwrap_or(-0.20),

            // Steering
            max_steering_angle_rad: car_toml.physics.max_steering_angle_rad,
            steering_ratio: car_toml.physics.steering_ratio.unwrap_or(14.0),

            // Suspension
            suspension: SuspensionConfig {
                spring_rate_front_n_per_m: suspension_toml
                    .spring_rate_front_n_per_m
                    .unwrap_or(suspension_defaults.spring_rate_front_n_per_m),
                spring_rate_rear_n_per_m: suspension_toml
                    .spring_rate_rear_n_per_m
                    .unwrap_or(suspension_defaults.spring_rate_rear_n_per_m),
                damper_compression_front: suspension_toml
                    .damper_compression_front
                    .unwrap_or(suspension_defaults.damper_compression_front),
                damper_compression_rear: suspension_toml
                    .damper_compression_rear
                    .unwrap_or(suspension_defaults.damper_compression_rear),
                damper_rebound_front: suspension_toml
                    .damper_rebound_front
                    .unwrap_or(suspension_defaults.damper_rebound_front),
                damper_rebound_rear: suspension_toml
                    .damper_rebound_rear
                    .unwrap_or(suspension_defaults.damper_rebound_rear),
                anti_roll_bar_front: suspension_toml
                    .anti_roll_bar_front
                    .unwrap_or(suspension_defaults.anti_roll_bar_front),
                anti_roll_bar_rear: suspension_toml
                    .anti_roll_bar_rear
                    .unwrap_or(suspension_defaults.anti_roll_bar_rear),
                max_travel_m: suspension_toml
                    .max_travel_m
                    .unwrap_or(suspension_defaults.max_travel_m),
            },

            // Tires
            tire_config: TireConfig {
                grip_coefficient: car_toml.physics.grip_coefficient,
                ..TireConfig::default()
            },
        };

        Self::validate(&config, &path_str)?;
        Ok(config)
    }

    /// Reject configs that would panic or corrupt the simulation later
    /// (division by zero, indexing empty gear tables, NaN propagation).
    fn validate(config: &CarConfig, path: &str) -> Result<(), CarLoadError> {
        let mut problems = Vec::new();

        if !(config.mass_kg.is_finite() && config.mass_kg > 0.0) {
            problems.push(format!("mass_kg must be positive (got {})", config.mass_kg));
        }
        if !(config.wheelbase_m.is_finite() && config.wheelbase_m > 0.0) {
            problems.push(format!(
                "wheelbase_m must be positive (got {})",
                config.wheelbase_m
            ));
        }
        if config.gear_ratios.is_empty() {
            problems.push("gear_ratios must not be empty".to_string());
        }
        if !(config.max_steering_angle_rad.is_finite() && config.max_steering_angle_rad > 0.0) {
            problems.push(format!(
                "max_steering_angle_rad must be positive (got {})",
                config.max_steering_angle_rad
            ));
        }
        if !(config.max_brake_force_n.is_finite() && config.max_brake_force_n >= 0.0) {
            problems.push(format!(
                "max_brake_force_n must be non-negative (got {})",
                config.max_brake_force_n
            ));
        }
        if !(config.tire_config.grip_coefficient.is_finite()
            && config.tire_config.grip_coefficient > 0.0)
        {
            problems.push(format!(
                "grip_coefficient must be positive (got {})",
                config.tire_config.grip_coefficient
            ));
        }

        // Moddable dimension / aero / balance parameters
        let positive_finite = [
            ("height_m", config.height_m),
            ("track_width_front_m", config.track_width_front_m),
            ("track_width_rear_m", config.track_width_rear_m),
            ("wheel_radius_m", config.wheel_radius_m),
            ("cog_height_m", config.cog_height_m),
            ("frontal_area_m2", config.frontal_area_m2),
            (
                "suspension.spring_rate_front_n_per_m",
                config.suspension.spring_rate_front_n_per_m,
            ),
            (
                "suspension.spring_rate_rear_n_per_m",
                config.suspension.spring_rate_rear_n_per_m,
            ),
            ("suspension.max_travel_m", config.suspension.max_travel_m),
        ];
        for (name, value) in positive_finite {
            if !(value.is_finite() && value > 0.0) {
                problems.push(format!("{} must be positive (got {})", name, value));
            }
        }
        if !(config.weight_distribution_front.is_finite()
            && config.weight_distribution_front > 0.0
            && config.weight_distribution_front < 1.0)
        {
            problems.push(format!(
                "weight_distribution_front must be in (0, 1) (got {})",
                config.weight_distribution_front
            ));
        }
        if !(config.brake_bias_front.is_finite()
            && config.brake_bias_front > 0.0
            && config.brake_bias_front < 1.0)
        {
            problems.push(format!(
                "brake_bias_front must be in (0, 1) (got {})",
                config.brake_bias_front
            ));
        }

        if problems.is_empty() {
            Ok(())
        } else {
            Err(CarLoadError::Invalid {
                path: path.to_string(),
                reason: problems.join("; "),
            })
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    /// Minimal valid TOML with only the required fields.
    const BASE_TOML: &str = r#"
id = "d4e5f6a7-b8c9-4d4e-1f2a-3b4c5d6e7f8a"
name = "Test Car"
version = "1.0.0"
model = "test.glb"

[physics]
mass_kg = 1000.0
max_engine_force_n = 5000.0
max_brake_force_n = 10000.0
drag_coefficient = 0.3
grip_coefficient = 1.0
max_steering_angle_rad = 0.5
wheelbase_m = 2.6
"#;

    fn load_toml_str(toml: &str) -> Result<CarConfig, CarLoadError> {
        let mut file = tempfile::NamedTempFile::new().expect("create temp file");
        file.write_all(toml.as_bytes()).expect("write temp file");
        CarLoader::load_from_file(file.path())
    }

    #[test]
    fn new_physics_and_suspension_fields_load_from_toml() {
        let toml = format!(
            "{BASE_TOML}
height_m = 1.1
track_width_front_m = 1.7
track_width_rear_m = 1.65
wheel_radius_m = 0.30
cog_height_m = 0.35
weight_distribution_front = 0.45
brake_bias_front = 0.55
frontal_area_m2 = 1.8
lift_coefficient_front = -0.5
lift_coefficient_rear = -0.9
steering_ratio = 11.0

[suspension]
spring_rate_front_n_per_m = 120000.0
spring_rate_rear_n_per_m = 110000.0
damper_compression_front = 3500.0
damper_compression_rear = 3300.0
damper_rebound_front = 5000.0
damper_rebound_rear = 4800.0
anti_roll_bar_front = 20000.0
anti_roll_bar_rear = 18000.0
max_travel_m = 0.10
"
        );
        let config = load_toml_str(&toml).expect("car with moddable fields should load");

        assert_eq!(config.height_m, 1.1);
        assert_eq!(config.track_width_front_m, 1.7);
        assert_eq!(config.track_width_rear_m, 1.65);
        assert_eq!(config.wheel_radius_m, 0.30);
        assert_eq!(config.cog_height_m, 0.35);
        assert_eq!(config.weight_distribution_front, 0.45);
        assert_eq!(config.brake_bias_front, 0.55);
        assert_eq!(config.frontal_area_m2, 1.8);
        assert_eq!(config.lift_coefficient_front, -0.5);
        assert_eq!(config.lift_coefficient_rear, -0.9);
        assert_eq!(config.steering_ratio, 11.0);

        assert_eq!(config.suspension.spring_rate_front_n_per_m, 120000.0);
        assert_eq!(config.suspension.spring_rate_rear_n_per_m, 110000.0);
        assert_eq!(config.suspension.damper_compression_front, 3500.0);
        assert_eq!(config.suspension.damper_compression_rear, 3300.0);
        assert_eq!(config.suspension.damper_rebound_front, 5000.0);
        assert_eq!(config.suspension.damper_rebound_rear, 4800.0);
        assert_eq!(config.suspension.anti_roll_bar_front, 20000.0);
        assert_eq!(config.suspension.anti_roll_bar_rear, 18000.0);
        assert_eq!(config.suspension.max_travel_m, 0.10);
    }

    #[test]
    fn missing_optional_fields_get_documented_defaults() {
        let config = load_toml_str(BASE_TOML).expect("minimal car should load");

        // Previously hardcoded values must remain the defaults.
        assert_eq!(config.height_m, 1.3);
        assert_eq!(config.track_width_front_m, 1.6);
        assert_eq!(config.track_width_rear_m, 1.58);
        assert_eq!(config.wheel_radius_m, 0.33);
        assert_eq!(config.cog_height_m, 0.45);
        assert_eq!(config.cog_offset_x_m, 0.0);
        assert_eq!(config.weight_distribution_front, 0.52);
        assert_eq!(config.brake_bias_front, 0.6);
        assert!(config.abs_enabled);
        assert_eq!(config.frontal_area_m2, 2.2);
        assert_eq!(config.lift_coefficient_front, -0.15);
        assert_eq!(config.lift_coefficient_rear, -0.20);
        assert_eq!(config.steering_ratio, 14.0);

        let defaults = SuspensionConfig::default();
        assert_eq!(
            config.suspension.spring_rate_front_n_per_m,
            defaults.spring_rate_front_n_per_m
        );
        assert_eq!(
            config.suspension.spring_rate_rear_n_per_m,
            defaults.spring_rate_rear_n_per_m
        );
        assert_eq!(
            config.suspension.damper_compression_front,
            defaults.damper_compression_front
        );
        assert_eq!(
            config.suspension.damper_compression_rear,
            defaults.damper_compression_rear
        );
        assert_eq!(
            config.suspension.damper_rebound_front,
            defaults.damper_rebound_front
        );
        assert_eq!(
            config.suspension.damper_rebound_rear,
            defaults.damper_rebound_rear
        );
        assert_eq!(
            config.suspension.anti_roll_bar_front,
            defaults.anti_roll_bar_front
        );
        assert_eq!(
            config.suspension.anti_roll_bar_rear,
            defaults.anti_roll_bar_rear
        );
        assert_eq!(config.suspension.max_travel_m, defaults.max_travel_m);
    }

    #[test]
    fn invalid_values_are_rejected() {
        // Negative mass
        let toml = BASE_TOML.replace("mass_kg = 1000.0", "mass_kg = -50.0");
        assert!(matches!(
            load_toml_str(&toml),
            Err(CarLoadError::Invalid { .. })
        ));

        // Brake bias out of (0, 1)
        let toml = format!("{BASE_TOML}\nbrake_bias_front = 1.5\n");
        assert!(matches!(
            load_toml_str(&toml),
            Err(CarLoadError::Invalid { .. })
        ));

        // Empty gear ratios
        let toml = format!("{BASE_TOML}\n[transmission]\ngear_ratios = []\n");
        assert!(matches!(
            load_toml_str(&toml),
            Err(CarLoadError::Invalid { .. })
        ));

        // Weight distribution out of (0, 1)
        let toml = format!("{BASE_TOML}\nweight_distribution_front = 0.0\n");
        assert!(matches!(
            load_toml_str(&toml),
            Err(CarLoadError::Invalid { .. })
        ));

        // Negative suspension spring rate
        let toml = format!("{BASE_TOML}\n[suspension]\nspring_rate_front_n_per_m = -1.0\n");
        assert!(matches!(
            load_toml_str(&toml),
            Err(CarLoadError::Invalid { .. })
        ));

        // Zero wheel radius
        let toml = format!("{BASE_TOML}\nwheel_radius_m = 0.0\n");
        assert!(matches!(
            load_toml_str(&toml),
            Err(CarLoadError::Invalid { .. })
        ));
    }

    #[test]
    fn real_content_cars_still_load() {
        // Skip gracefully on CI checkouts without the content folder.
        let candidates = [
            "../content/cars/golfcart/car.toml",
            "../content/cars/2021-f1-fugazzi-sf21/car.toml",
        ];
        for candidate in candidates {
            let path = Path::new(candidate);
            if !path.exists() {
                eprintln!("skipping {candidate}: content not present");
                return;
            }
            let config = CarLoader::load_from_file(path)
                .unwrap_or_else(|e| panic!("{candidate} should load: {e}"));
            assert!(config.mass_kg > 0.0);
            // Content cars don't set the new fields yet, so they must keep
            // the previously hardcoded defaults.
            assert_eq!(config.height_m, 1.3);
            assert_eq!(config.brake_bias_front, 0.6);
            assert_eq!(
                config.suspension.spring_rate_front_n_per_m,
                SuspensionConfig::default().spring_rate_front_n_per_m
            );
        }
    }
}
