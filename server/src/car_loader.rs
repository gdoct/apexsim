use crate::data::*;
use serde::Deserialize;
use std::path::Path;
use tracing::{debug, warn};
use uuid::Uuid;

#[derive(Debug, Deserialize)]
struct CarToml {
    id: String,
    name: String,
    #[allow(dead_code)]
    version: String,
    model: String,
    #[serde(default)]
    class: String,
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
    #[serde(default)]
    tires: Option<TiresToml>,
    #[serde(default)]
    aero: Option<AeroToml>,
    /// `[[livery]]` tables: only the names matter here (the client paints).
    #[serde(default)]
    livery: Vec<LiveryToml>,
}

#[derive(Debug, Deserialize)]
struct LiveryToml {
    name: String,
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
    /// DRS: the fractions the open flap takes off the drag and the rear
    /// downforce. Absent, an F1 car gets `DrsSpec::F1` and any other class
    /// no DRS at all; `drs_drag_reduction = 0` switches it off explicitly.
    #[serde(default)]
    drs_drag_reduction: Option<f32>,
    #[serde(default)]
    drs_rear_downforce_reduction: Option<f32>,
}

/// The car's DRS from its `[physics]` table, or the class default.
fn drs_spec(class: &str, drag: Option<f32>, rear: Option<f32>) -> Option<crate::data::DrsSpec> {
    let base = if class.eq_ignore_ascii_case("f1") {
        Some(crate::data::DrsSpec::F1)
    } else {
        None
    };
    match (drag, rear) {
        (None, None) => base,
        (drag, rear) => {
            let drag = drag.unwrap_or(base.map_or(0.0, |b| b.drag_reduction));
            let rear = rear.unwrap_or(base.map_or(0.0, |b| b.rear_downforce_reduction));
            (drag > 0.0).then_some(crate::data::DrsSpec {
                drag_reduction: drag.clamp(0.0, 0.5),
                rear_downforce_reduction: rear.clamp(0.0, 0.8),
            })
        }
    }
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

/// Optional `[aero]` section: how the downforce answers ride height and
/// rake (`crate::aero`). Without it the downforce is the constant the
/// lift coefficients give, as it always was.
#[derive(Debug, Deserialize, Default)]
#[serde(deny_unknown_fields)]
struct AeroToml {
    #[serde(default)]
    ride_height_front_m: Option<f32>,
    #[serde(default)]
    ride_height_rear_m: Option<f32>,
    /// Share of the downforce gained per centimetre lower.
    #[serde(default)]
    ride_height_sensitivity: Option<f32>,
    /// Front balance shift per centimetre more rake.
    #[serde(default)]
    rake_sensitivity: Option<f32>,
    #[serde(default)]
    stall_height_m: Option<f32>,
}

/// Optional `[tires]` section: what the one `grip_coefficient` cannot say
/// about a tyre. Every key defaults to the tyre every car had before it
/// (180 kPa at a 180 kPa optimum, linear in load, one compound, a friction
/// circle), so a car without the table simulates to the bit as it did. An
/// axle's own key (`load_sensitivity_front`) wins over the shared one.
#[derive(Debug, Deserialize, Default)]
#[serde(deny_unknown_fields)]
struct TiresToml {
    #[serde(default)]
    optimal_pressure_kpa: Option<f32>,
    #[serde(default)]
    pressure_front_kpa: Option<f32>,
    #[serde(default)]
    pressure_rear_kpa: Option<f32>,
    #[serde(default)]
    load_sensitivity: Option<f32>,
    #[serde(default)]
    load_sensitivity_front: Option<f32>,
    #[serde(default)]
    load_sensitivity_rear: Option<f32>,
    #[serde(default)]
    reference_load_n: Option<f32>,
    #[serde(default)]
    reference_load_front_n: Option<f32>,
    #[serde(default)]
    reference_load_rear_n: Option<f32>,
    #[serde(default)]
    longitudinal_grip_factor: Option<f32>,
    #[serde(default)]
    front_grip_scale: Option<f32>,
    #[serde(default)]
    rear_grip_scale: Option<f32>,
    /// The compound's working window (`crate::tyre_thermal`): its middle,
    /// half-width and the grip lost per degree outside it, °C.
    #[serde(default)]
    optimal_temperature_c: Option<f32>,
    #[serde(default)]
    temperature_window_c: Option<f32>,
    #[serde(default)]
    temperature_grip_falloff: Option<f32>,
    /// Tyre blankets: what the car goes out at, °C. Absent: the air.
    #[serde(default)]
    blanket_temperature_c: Option<f32>,
}

/// Optional `[engine.turbo]`: the lag of a turbo whose boost the torque
/// curve already carries. Absent, the curve arrives with the throttle.
#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
struct TurboToml {
    boosted_share: f32,
    #[serde(default)]
    lag_up_s: Option<f32>,
    #[serde(default)]
    lag_down_s: Option<f32>,
}

/// Spool times for an `[engine.turbo]` that names only its boost: a
/// modern single turbo, a quarter second to build and a little longer to
/// bleed off.
const DEFAULT_TURBO_LAG_UP_S: f32 = 0.25;
const DEFAULT_TURBO_LAG_DOWN_S: f32 = 0.4;

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
    #[serde(default)]
    turbo: Option<TurboToml>,
    /// Turbo- or supercharged (the air's density costs it little). Absent:
    /// whether the car has an `[engine.turbo]` table.
    #[serde(default)]
    forced_induction: Option<bool>,
}

#[derive(Debug, Deserialize, Default)]
struct TransmissionToml {
    #[serde(default)]
    transmission_type: Option<String>,
    #[serde(default)]
    gear_ratios: Option<Vec<f32>>,
    #[serde(default)]
    ratio_curve: Option<RatioCurveToml>,
    #[serde(default)]
    final_drive_ratio: Option<f32>,
    #[serde(default)]
    shift_time_s: Option<f32>,
    #[serde(default)]
    efficiency: Option<f32>,
}

/// A gearbox described by its two end ratios and the shape of the
/// progression between them, instead of a hand-typed ladder.
///
/// It is the shape that makes a box right or wrong: hand-typed ladders
/// drift into ratios nothing checks, and a single wrong number there means
/// a gear the engine cannot pull. Here the only numbers to get right are
/// the two ends, and each is a road speed you can read off the car.
#[derive(Debug, Deserialize, Default)]
struct RatioCurveToml {
    gears: usize,
    first: f32,
    top: f32,
    #[serde(default)]
    shape: Option<String>,
    #[serde(default)]
    reverse: Option<f32>,
}

/// How the ratios between first and top are spaced.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RatioCurveShape {
    /// Equal steps: every shift drops the same fraction of the revs. The
    /// neutral choice, and what a two-point curve means if unqualified.
    Geometric,
    /// Steps that narrow towards top gear — a long first for getting off
    /// the line, tight ratios up top where the car lives in the power band.
    /// What a racing box actually does.
    Progressive,
}

impl RatioCurveShape {
    fn parse(name: &str) -> Option<Self> {
        match name.to_ascii_lowercase().as_str() {
            "geometric" | "constant" => Some(Self::Geometric),
            "progressive" => Some(Self::Progressive),
            _ => None,
        }
    }

    /// Exponent warping the progression. 1.0 is a pure geometric ladder;
    /// below 1.0 the early steps grow and the late ones shrink, which is
    /// what "progressive" means on a gearbox spec sheet.
    fn exponent(self) -> f32 {
        match self {
            Self::Geometric => 1.0,
            Self::Progressive => 0.75,
        }
    }
}

/// Road speed in m/s at `rpm` through a gear ratio, the same drivetrain
/// arithmetic the sim uses to turn wheel speed back into engine rpm.
pub fn geared_speed_mps(config: &CarConfig, ratio: f32, rpm: f32) -> f32 {
    let total = ratio.abs() * config.final_drive_ratio;
    if total < 1e-3 {
        return 0.0;
    }
    (rpm / 60.0) * 2.0 * std::f32::consts::PI * config.wheel_radius_m / total
}

/// The speed the car can actually drag itself to on full power, m/s, from
/// the same aero model the sim runs (`P = ½ρ·Cd·A·v³`). Hybrid assist
/// counts: it is there on the straight where top gear matters.
pub fn drag_limited_speed_mps(config: &CarConfig) -> f32 {
    let hybrid_w = if config.hybrid.enabled {
        config.hybrid.motor_max_power_kw * 1000.0
    } else {
        0.0
    };
    let power_w = (config.max_engine_power_w + hybrid_w) * config.transmission.efficiency;
    let drag_area = crate::physics::AIR_DENSITY * config.drag_coefficient * config.frontal_area_m2;
    if power_w <= 0.0 || drag_area <= 0.0 {
        return f32::INFINITY;
    }
    (2.0 * power_w / drag_area).cbrt()
}

/// How far past the drag-limited speed a top gear may be geared before the
/// gearing is more decoration than transmission. A little over is normal —
/// a car should be pulling near the limiter in top, not bouncing off it —
/// but a top gear good for twice the car's terminal speed means every gear
/// below it is too long to use.
const TOP_GEAR_OVERRUN_LIMIT: f32 = 1.35;

/// Gearing nobody can drive is not a load failure — a modder is allowed an
/// odd box, and the sim runs it fine — but it is never intentional, so say
/// so loudly with the numbers that show it.
fn warn_about_unusable_gearing(config: &CarConfig, path: &str) {
    let Some(top) = config.gear_ratios.iter().skip(1).copied().last() else {
        return;
    };
    let top_speed_mps = geared_speed_mps(config, top, config.redline_rpm);
    let drag_limit_mps = drag_limited_speed_mps(config);
    if !drag_limit_mps.is_finite() || top_speed_mps <= drag_limit_mps * TOP_GEAR_OVERRUN_LIMIT {
        return;
    }
    warn!(
        "{}: top gear is geared for {:.0} km/h at the redline but {} can only reach          {:.0} km/h; every gear is too long and the engine will never reach the limiter",
        path,
        top_speed_mps * 3.6,
        config.name,
        drag_limit_mps * 3.6,
    );
}

/// The most gears a curve may generate. Gear numbers are `i8` on the wire
/// and a real sequential box tops out at eight; this is only here so a typo
/// cannot ask for a million-speed gearbox.
const MAX_CURVE_GEARS: usize = 12;

/// Build the gear ladder from a ratio curve: reverse first (negative), then
/// `gears` forward ratios from `first` down to `top`.
///
/// The ratios are spaced evenly in *log* space — the space a gearbox is
/// actually designed in, where a step is the fraction of the revs a shift
/// drops — warped by the shape's exponent so a progressive box gets its
/// long first gear and its tight top end.
pub fn gear_ratios_from_curve(
    gears: usize,
    first: f32,
    top: f32,
    shape: RatioCurveShape,
    reverse: Option<f32>,
) -> Result<Vec<f32>, String> {
    if gears == 0 || gears > MAX_CURVE_GEARS {
        return Err(format!(
            "transmission.ratio_curve.gears must be 1..={MAX_CURVE_GEARS} (got {gears})"
        ));
    }
    for (name, value) in [("first", first), ("top", top)] {
        if !(value.is_finite() && value > 0.0) {
            return Err(format!(
                "transmission.ratio_curve.{name} must be positive (got {value})"
            ));
        }
    }
    if gears > 1 && first <= top {
        return Err(format!(
            "transmission.ratio_curve.first ({first}) must be taller than top ({top});              ratios count down"
        ));
    }
    // Reverse is a reverse gear whichever sign it was written with; absent,
    // it matches first, which is how a real box is laid out.
    let reverse = -reverse.unwrap_or(first).abs();
    if !reverse.is_finite() || reverse == 0.0 {
        return Err("transmission.ratio_curve.reverse must be a non-zero ratio".to_string());
    }

    let mut ratios = Vec::with_capacity(gears + 1);
    ratios.push(reverse);
    if gears == 1 {
        ratios.push(first);
        return Ok(ratios);
    }
    let exponent = shape.exponent();
    let span = (top / first).ln();
    for gear in 0..gears {
        let t = gear as f32 / (gears - 1) as f32;
        ratios.push(first * (span * t.powf(exponent)).exp());
    }
    Ok(ratios)
}

#[derive(Debug, Deserialize, Default)]
struct DrivetrainToml {
    #[serde(default)]
    layout: Option<String>,
    /// Share of an AWD car's drive to the front axle; 0.4 when absent.
    #[serde(default)]
    awd_front_share: Option<f32>,
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
    #[serde(default)]
    simulated: Option<bool>,
}

#[derive(Debug, Deserialize, Default)]
struct FuelToml {
    #[serde(default)]
    capacity_liters: Option<f32>,
    #[serde(default)]
    idle_consumption_lps: Option<f32>,
    #[serde(default)]
    load_consumption_scale: Option<f32>,
    #[serde(default)]
    thermal_efficiency: Option<f32>,
    #[serde(default)]
    density_kg_per_l: Option<f32>,
    #[serde(default)]
    tank_front_share: Option<f32>,
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

/// The subfolders of a cars folder, in the order they are read: the shipped
/// cars, then the player's own (imported or hand-made, gitignored). The
/// first car to claim an id wins, so a custom car can never replace a
/// shipped one by reusing its id.
pub const CAR_DIRS: [&str; 2] = ["default", "custom"];

/// Every `car.toml` under a cars folder (`content/cars`, or `content/cars`
/// beside a packaged server): `default/` then `custom/`, each searched at
/// any depth and sorted, so the order never depends on the filesystem. A
/// folder with neither subfolder (an older layout, or a folder of cars
/// passed by hand) is searched as it is.
pub fn car_toml_paths(cars_dir: &Path) -> Vec<std::path::PathBuf> {
    fn walk(dir: &Path, out: &mut Vec<std::path::PathBuf>) {
        let Ok(entries) = std::fs::read_dir(dir) else {
            return;
        };
        let mut paths: Vec<_> = entries.filter_map(|e| e.ok()).map(|e| e.path()).collect();
        paths.sort();
        for path in paths {
            if path.is_dir() {
                walk(&path, out);
            } else if path.file_name().and_then(|s| s.to_str()) == Some("car.toml") {
                out.push(path);
            }
        }
    }
    let split: Vec<_> = CAR_DIRS
        .iter()
        .map(|sub| cars_dir.join(sub))
        .filter(|dir| dir.is_dir())
        .collect();
    let mut out = Vec::new();
    if split.is_empty() {
        walk(cars_dir, &mut out);
    } else {
        for dir in split {
            walk(&dir, &mut out);
        }
    }
    out
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
        let tires_toml = car_toml.tires.unwrap_or_default();
        let aero_toml = car_toml.aero.unwrap_or_default();
        let aero_defaults = crate::aero::AeroConfig::default();
        let tire_defaults = TireConfig::default();
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

        // Gearing: either a hand-typed ladder or a ratio curve to generate
        // one from, never both — a car carrying two answers has no answer.
        let gear_ratios = match (
            transmission_toml.gear_ratios,
            &transmission_toml.ratio_curve,
        ) {
            (Some(_), Some(_)) => {
                return Err(CarLoadError::Invalid {
                    path: path_str.clone(),
                    reason: "transmission has both gear_ratios and ratio_curve; keep one"
                        .to_string(),
                })
            }
            (None, Some(curve)) => {
                let shape = match curve.shape.as_deref() {
                    None => RatioCurveShape::Geometric,
                    Some(name) => RatioCurveShape::parse(name).ok_or_else(|| {
                        CarLoadError::Invalid {
                            path: path_str.clone(),
                            reason: format!(
                                "unknown transmission.ratio_curve.shape {name:?}                                  (geometric or progressive)"
                            ),
                        }
                    })?,
                };
                gear_ratios_from_curve(curve.gears, curve.first, curve.top, shape, curve.reverse)
                    .map_err(|reason| CarLoadError::Invalid {
                        path: path_str.clone(),
                        reason,
                    })?
            }
            (Some(ratios), None) => ratios,
            (None, None) => vec![-3.5, 3.8, 2.4, 1.7, 1.3, 1.0, 0.8],
        };

        let drs = drs_spec(
            &car_toml.class,
            car_toml.physics.drs_drag_reduction,
            car_toml.physics.drs_rear_downforce_reduction,
        );
        let mut config = CarConfig {
            id,
            name: car_toml.name,
            model: car_toml.model,
            class: car_toml.class,
            content_crc: crate::content_crc::content_crc(content.as_bytes()),
            livery_names: car_toml.livery.into_iter().map(|l| l.name).collect(),

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
            gear_ratios,
            final_drive_ratio: transmission_toml.final_drive_ratio.unwrap_or(3.7),
            drivetrain: match drivetrain_toml.layout.as_deref() {
                Some("FWD") | Some("fwd") => Drivetrain::FWD,
                Some("AWD") | Some("awd") | Some("4WD") | Some("4wd") => Drivetrain::AWD,
                _ => Drivetrain::RWD,
            },
            awd_front_share: drivetrain_toml
                .awd_front_share
                .unwrap_or(DEFAULT_AWD_FRONT_SHARE),

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
                forced_induction: engine_toml
                    .forced_induction
                    .unwrap_or(engine_toml.turbo.is_some()),
                turbo: engine_toml.turbo.map(|t| TurboConfig {
                    boosted_share: t.boosted_share,
                    lag_up_s: t.lag_up_s.unwrap_or(DEFAULT_TURBO_LAG_UP_S),
                    lag_down_s: t.lag_down_s.unwrap_or(DEFAULT_TURBO_LAG_DOWN_S),
                }),
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
                simulated: differential_toml.simulated.unwrap_or(false),
            },
            fuel: FuelConfig {
                capacity_liters: fuel_toml.capacity_liters.unwrap_or(100.0),
                idle_consumption_lps: fuel_toml.idle_consumption_lps.unwrap_or(0.00005),
                // The old throttle-times-revs rule only for a file that
                // names its scale and not the engine's efficiency.
                load_consumption_scale: fuel_toml
                    .load_consumption_scale
                    .filter(|_| fuel_toml.thermal_efficiency.is_none()),
                thermal_efficiency: fuel_toml
                    .thermal_efficiency
                    .unwrap_or(FuelConfig::default().thermal_efficiency),
                density_kg_per_l: fuel_toml
                    .density_kg_per_l
                    .unwrap_or(FuelConfig::default().density_kg_per_l),
                tank_front_share: fuel_toml.tank_front_share,
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
            drs,
            aero: aero_defaults,

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
                optimal_pressure_kpa: tires_toml
                    .optimal_pressure_kpa
                    .unwrap_or(tire_defaults.optimal_pressure_kpa),
                // Running pressures default to the optimum, so a car that
                // names only its optimum runs at it.
                pressure_front_kpa: tires_toml.pressure_front_kpa.unwrap_or(
                    tires_toml
                        .optimal_pressure_kpa
                        .unwrap_or(tire_defaults.pressure_front_kpa),
                ),
                pressure_rear_kpa: tires_toml.pressure_rear_kpa.unwrap_or(
                    tires_toml
                        .optimal_pressure_kpa
                        .unwrap_or(tire_defaults.pressure_rear_kpa),
                ),
                load_sensitivity_front: tires_toml
                    .load_sensitivity_front
                    .or(tires_toml.load_sensitivity)
                    .unwrap_or(tire_defaults.load_sensitivity_front),
                load_sensitivity_rear: tires_toml
                    .load_sensitivity_rear
                    .or(tires_toml.load_sensitivity)
                    .unwrap_or(tire_defaults.load_sensitivity_rear),
                reference_load_front_n: tires_toml
                    .reference_load_front_n
                    .or(tires_toml.reference_load_n),
                reference_load_rear_n: tires_toml
                    .reference_load_rear_n
                    .or(tires_toml.reference_load_n),
                longitudinal_grip_factor: tires_toml
                    .longitudinal_grip_factor
                    .unwrap_or(tire_defaults.longitudinal_grip_factor),
                front_grip_scale: tires_toml
                    .front_grip_scale
                    .unwrap_or(tire_defaults.front_grip_scale),
                rear_grip_scale: tires_toml
                    .rear_grip_scale
                    .unwrap_or(tire_defaults.rear_grip_scale),
                optimal_temperature_c: tires_toml
                    .optimal_temperature_c
                    .unwrap_or(tire_defaults.optimal_temperature_c),
                temperature_window_c: tires_toml
                    .temperature_window_c
                    .unwrap_or(tire_defaults.temperature_window_c),
                temperature_grip_falloff: tires_toml
                    .temperature_grip_falloff
                    .unwrap_or(tire_defaults.temperature_grip_falloff),
                blanket_temperature_c: tires_toml.blanket_temperature_c,
                ..tire_defaults
            },
        };

        config.aero = crate::aero::AeroConfig {
            ride_height_front_m: aero_toml
                .ride_height_front_m
                .unwrap_or(aero_defaults.ride_height_front_m),
            ride_height_rear_m: aero_toml
                .ride_height_rear_m
                .unwrap_or(aero_defaults.ride_height_rear_m),
            ride_height_sensitivity: aero_toml
                .ride_height_sensitivity
                .unwrap_or(aero_defaults.ride_height_sensitivity),
            rake_sensitivity: aero_toml
                .rake_sensitivity
                .unwrap_or(aero_defaults.rake_sensitivity),
            stall_height_m: aero_toml
                .stall_height_m
                .unwrap_or(aero_defaults.stall_height_m),
            ..aero_defaults
        };
        Self::validate(&config, &path_str)?;
        // Where the map is zero: the car on its stock springs at speed.
        crate::aero::fit_reference(&mut config);
        warn_about_unusable_gearing(&config, &path_str);
        Ok(config)
    }

    /// Reject configs that would panic or corrupt the simulation later
    /// (division by zero, indexing empty gear tables, NaN propagation).
    fn validate(config: &CarConfig, path: &str) -> Result<(), CarLoadError> {
        let mut problems = Vec::new();

        if !(config.mass_kg.is_finite() && config.mass_kg > 0.0) {
            problems.push(format!("mass_kg must be positive (got {})", config.mass_kg));
        }
        let fuel = &config.fuel;
        if !(fuel.capacity_liters.is_finite() && fuel.capacity_liters >= 0.0) {
            problems.push(format!(
                "[fuel] capacity_liters must not be negative (got {})",
                fuel.capacity_liters
            ));
        }
        if !(fuel.thermal_efficiency.is_finite() && (0.05..=0.7).contains(&fuel.thermal_efficiency))
        {
            problems.push(format!(
                "[fuel] thermal_efficiency must be within 0.05-0.7 (got {})",
                fuel.thermal_efficiency
            ));
        }
        if !(fuel.density_kg_per_l.is_finite() && (0.5..=1.0).contains(&fuel.density_kg_per_l)) {
            problems.push(format!(
                "[fuel] density_kg_per_l must be within 0.5-1.0 (got {})",
                fuel.density_kg_per_l
            ));
        }
        if let Some(share) = fuel.tank_front_share {
            if !(share.is_finite() && (0.0..=1.0).contains(&share)) {
                problems.push(format!(
                    "[fuel] tank_front_share must be within 0-1 (got {share})"
                ));
            }
        }
        if !(config.wheelbase_m.is_finite() && config.wheelbase_m > 0.0) {
            problems.push(format!(
                "wheelbase_m must be positive (got {})",
                config.wheelbase_m
            ));
        }
        if config.gear_ratios.is_empty() {
            problems.push("gear_ratios must not be empty".to_string());
        } else {
            // Index 0 is reverse and the rest count down: physics indexes
            // this table by gear number and reads the sign as direction, so
            // a ladder out of order is a car that shifts into the wrong gear.
            if !(config.gear_ratios[0].is_finite() && config.gear_ratios[0] < 0.0) {
                problems.push(format!(
                    "gear_ratios[0] is reverse and must be negative (got {})",
                    config.gear_ratios[0]
                ));
            }
            for (i, ratio) in config.gear_ratios.iter().enumerate().skip(1) {
                if !(ratio.is_finite() && *ratio > 0.0) {
                    problems.push(format!(
                        "gear_ratios[{i}] must be a positive ratio (got {ratio})"
                    ));
                }
            }
            let forward = &config.gear_ratios[1..];
            if let Some(i) = forward.windows(2).position(|w| w[0] <= w[1]) {
                problems.push(format!(
                    "gear ratios must count down: gear {} ({}) is not taller than gear {} ({})",
                    i + 1,
                    forward[i],
                    i + 2,
                    forward[i + 1]
                ));
            }
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

        // Tyres, drivetrain and turbo: each range is wide enough for any
        // real car (an Assetto Corsa import's figures included) and narrow
        // enough that a typo cannot make a tyre with no grip or a turbo
        // that is all boost.
        let tyre = &config.tire_config;
        let mut in_range = |name: &str, value: f32, lo: f32, hi: f32| {
            if !(value.is_finite() && value >= lo && value <= hi) {
                problems.push(format!("{name} must be in [{lo}, {hi}] (got {value})"));
            }
        };
        in_range(
            "tires.optimal_pressure_kpa",
            tyre.optimal_pressure_kpa,
            50.0,
            400.0,
        );
        in_range(
            "tires.pressure_front_kpa",
            tyre.pressure_front_kpa,
            50.0,
            400.0,
        );
        in_range(
            "tires.pressure_rear_kpa",
            tyre.pressure_rear_kpa,
            50.0,
            400.0,
        );
        in_range(
            "tires.load_sensitivity_front",
            tyre.load_sensitivity_front,
            0.3,
            1.2,
        );
        in_range(
            "tires.load_sensitivity_rear",
            tyre.load_sensitivity_rear,
            0.3,
            1.2,
        );
        for (name, reference) in [
            ("tires.reference_load_front_n", tyre.reference_load_front_n),
            ("tires.reference_load_rear_n", tyre.reference_load_rear_n),
        ] {
            if let Some(load) = reference {
                in_range(name, load, 100.0, 30000.0);
            }
        }
        in_range(
            "tires.longitudinal_grip_factor",
            tyre.longitudinal_grip_factor,
            0.6,
            1.6,
        );
        in_range("tires.front_grip_scale", tyre.front_grip_scale, 0.5, 1.5);
        in_range("tires.rear_grip_scale", tyre.rear_grip_scale, 0.5, 1.5);
        let aero = &config.aero;
        in_range(
            "aero.ride_height_front_m",
            aero.ride_height_front_m,
            0.005,
            0.3,
        );
        in_range(
            "aero.ride_height_rear_m",
            aero.ride_height_rear_m,
            0.005,
            0.3,
        );
        in_range(
            "aero.ride_height_sensitivity",
            aero.ride_height_sensitivity,
            0.0,
            0.1,
        );
        in_range("aero.rake_sensitivity", aero.rake_sensitivity, 0.0, 0.05);
        in_range("aero.stall_height_m", aero.stall_height_m, 0.0, 0.1);
        in_range(
            "tires.optimal_temperature_c",
            tyre.optimal_temperature_c,
            30.0,
            150.0,
        );
        in_range(
            "tires.temperature_window_c",
            tyre.temperature_window_c,
            0.0,
            50.0,
        );
        in_range(
            "tires.temperature_grip_falloff",
            tyre.temperature_grip_falloff,
            0.0,
            0.03,
        );
        if let Some(blanket) = tyre.blanket_temperature_c {
            in_range("tires.blanket_temperature_c", blanket, 0.0, 120.0);
        }
        in_range(
            "drivetrain.awd_front_share",
            config.awd_front_share,
            0.0,
            1.0,
        );
        let diff = &config.differential;
        in_range("differential.preload_nm", diff.preload_nm, 0.0, 5000.0);
        in_range("differential.lock_power", diff.lock_power, 0.0, 1.0);
        in_range("differential.lock_coast", diff.lock_coast, 0.0, 1.0);
        if let Some(turbo) = &config.engine.turbo {
            in_range("engine.turbo.boosted_share", turbo.boosted_share, 0.0, 0.9);
            in_range("engine.turbo.lag_up_s", turbo.lag_up_s, 0.0, 5.0);
            in_range("engine.turbo.lag_down_s", turbo.lag_down_s, 0.0, 5.0);
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
            "../content/cars/default/posh-gt3rs/car.toml",
            "../content/cars/default/fugazzi-sf26/car.toml",
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
            // The checksum a client compares against is the file's, so the
            // car catalog importer can reproduce it from the same bytes.
            let bytes = std::fs::read(path).unwrap();
            assert_eq!(
                config.content_crc,
                crate::content_crc::content_crc(&bytes),
                "{candidate}: content_crc is not the file's checksum"
            );
            assert_ne!(config.content_crc, 0);
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

    fn steps(ratios: &[f32]) -> Vec<f32> {
        ratios[1..].windows(2).map(|w| w[0] / w[1]).collect()
    }

    #[test]
    fn ratio_curve_hits_both_ends_exactly() {
        for shape in [RatioCurveShape::Geometric, RatioCurveShape::Progressive] {
            let ratios = gear_ratios_from_curve(8, 4.9, 1.8, shape, None).unwrap();
            assert_eq!(ratios.len(), 9, "reverse plus eight");
            assert!((ratios[1] - 4.9).abs() < 1e-4);
            assert!((ratios[8] - 1.8).abs() < 1e-4);
            assert!(
                ratios[1..].windows(2).all(|w| w[0] > w[1]),
                "{shape:?} must count down: {ratios:?}"
            );
        }
    }

    #[test]
    fn geometric_curve_drops_the_same_revs_every_shift() {
        let ratios = gear_ratios_from_curve(6, 3.6, 0.9, RatioCurveShape::Geometric, None).unwrap();
        let steps = steps(&ratios);
        for step in &steps {
            assert!((step - steps[0]).abs() < 1e-4, "uneven steps {steps:?}");
        }
    }

    #[test]
    fn progressive_curve_closes_up_towards_top_gear() {
        let ratios =
            gear_ratios_from_curve(8, 4.9, 1.8, RatioCurveShape::Progressive, None).unwrap();
        let steps = steps(&ratios);
        assert!(
            steps.windows(2).all(|w| w[0] > w[1]),
            "each shift should drop fewer revs than the one before: {steps:?}"
        );
    }

    #[test]
    fn ratio_curve_reverse_defaults_to_first_and_is_always_negative() {
        let shape = RatioCurveShape::Geometric;
        assert_eq!(
            gear_ratios_from_curve(5, 3.0, 1.0, shape, None).unwrap()[0],
            -3.0
        );
        assert_eq!(
            gear_ratios_from_curve(5, 3.0, 1.0, shape, Some(3.4)).unwrap()[0],
            -3.4
        );
        assert_eq!(
            gear_ratios_from_curve(5, 3.0, 1.0, shape, Some(-3.4)).unwrap()[0],
            -3.4
        );
        // A single-speed box is just its one ratio.
        assert_eq!(
            gear_ratios_from_curve(1, 9.0, 9.0, shape, None).unwrap(),
            vec![-9.0, 9.0]
        );
    }

    #[test]
    fn ratio_curve_rejects_nonsense() {
        let shape = RatioCurveShape::Geometric;
        assert!(gear_ratios_from_curve(0, 3.0, 1.0, shape, None).is_err());
        assert!(gear_ratios_from_curve(MAX_CURVE_GEARS + 1, 3.0, 1.0, shape, None).is_err());
        assert!(
            gear_ratios_from_curve(6, 1.0, 3.0, shape, None).is_err(),
            "inverted"
        );
        assert!(
            gear_ratios_from_curve(6, 2.0, 2.0, shape, None).is_err(),
            "flat"
        );
        assert!(gear_ratios_from_curve(6, 3.0, 0.0, shape, None).is_err());
        assert!(gear_ratios_from_curve(6, f32::NAN, 1.0, shape, None).is_err());
        assert!(gear_ratios_from_curve(6, 3.0, 1.0, shape, Some(0.0)).is_err());
    }

    #[test]
    fn ratio_curve_loads_from_toml() {
        let toml = format!(
            "{BASE_TOML}
[transmission]
final_drive_ratio = 3.5
efficiency = 0.9

[transmission.ratio_curve]
gears = 6
first = 3.6
top = 0.9
shape = \"Progressive\"
reverse = 3.2
"
        );
        let config = load_toml_str(&toml).expect("ratio curve should load");
        assert_eq!(config.gear_ratios.len(), 7);
        assert_eq!(config.gear_ratios[0], -3.2);
        assert!((config.gear_ratios[1] - 3.6).abs() < 1e-4);
        assert!((config.gear_ratios[6] - 0.9).abs() < 1e-4);
        // The keys around the sub-table still land on the transmission.
        assert_eq!(config.final_drive_ratio, 3.5);
        assert_eq!(config.transmission.efficiency, 0.9);
    }

    #[test]
    fn gearing_must_be_unambiguous_and_in_order() {
        let both = format!(
            "{BASE_TOML}
[transmission]
gear_ratios = [-3.0, 3.0, 2.0]

[transmission.ratio_curve]
gears = 2
first = 3.0
top = 2.0
"
        );
        assert!(matches!(
            load_toml_str(&both),
            Err(CarLoadError::Invalid { .. })
        ));

        let bad_shape = format!(
            "{BASE_TOML}
[transmission.ratio_curve]
gears = 4
first = 3.0
top = 1.0
shape = \"wobbly\"
"
        );
        assert!(matches!(
            load_toml_str(&bad_shape),
            Err(CarLoadError::Invalid { .. })
        ));

        let out_of_order = format!(
            "{BASE_TOML}
[transmission]
gear_ratios = [-3.0, 2.0, 3.0]
"
        );
        assert!(matches!(
            load_toml_str(&out_of_order),
            Err(CarLoadError::Invalid { .. })
        ));

        let forward_reverse = format!(
            "{BASE_TOML}
[transmission]
gear_ratios = [3.0, 3.0, 2.0]
"
        );
        assert!(matches!(
            load_toml_str(&forward_reverse),
            Err(CarLoadError::Invalid { .. })
        ));
    }

    /// Every shipped car must be geared for speeds it can actually reach.
    /// This is the check that would have caught both F1 cars being geared
    /// for 620 km/h: first gear ran to 190 and the engine never saw the
    /// limiter above fourth.
    #[test]
    fn shipped_cars_are_geared_for_speeds_they_can_reach() {
        let dir = Path::new("../content/cars/default");
        let paths = car_toml_paths(dir);
        if paths.is_empty() {
            eprintln!("skipping: content not present");
            return;
        }
        let mut checked = 0;
        for path in paths {
            let config = CarLoader::load_from_file(&path)
                .unwrap_or_else(|e| panic!("{} should load: {e}", path.display()));
            let top = *config.gear_ratios.last().unwrap();
            let top_speed = geared_speed_mps(&config, top, config.redline_rpm);
            let reachable = drag_limited_speed_mps(&config);
            assert!(
                top_speed <= reachable * TOP_GEAR_OVERRUN_LIMIT,
                "{}: top gear runs to {:.0} km/h, the car can reach {:.0}",
                config.name,
                top_speed * 3.6,
                reachable * 3.6
            );
            checked += 1;
        }
        assert!(checked > 0, "no cars found under {}", dir.display());
    }

    /// The tables an Assetto Corsa import writes, every key set.
    const IMPORT_TABLES: &str = r#"
[drivetrain]
layout = "AWD"
awd_front_share = 0.3

[differential]
differential_type = "ClutchLSD"
preload_nm = 50.0
lock_power = 0.4
lock_coast = 0.2
simulated = true

[tires]
optimal_pressure_kpa = 179.0
pressure_front_kpa = 182.0
load_sensitivity = 0.85
load_sensitivity_rear = 0.9
reference_load_n = 3768.0
longitudinal_grip_factor = 1.1
front_grip_scale = 0.98
rear_grip_scale = 1.02

[engine]
max_torque_nm = 500.0

[engine.turbo]
boosted_share = 0.35
lag_up_s = 0.6
"#;

    #[test]
    fn tyre_drivetrain_and_turbo_keys_load_from_toml() {
        let config = load_toml_str(&format!("{BASE_TOML}{IMPORT_TABLES}"))
            .expect("car with the import tables should load");
        assert_eq!(config.drivetrain, Drivetrain::AWD);
        assert_eq!(config.awd_front_share, 0.3);
        assert!(config.differential.simulated);
        assert_eq!(config.differential.lock_power, 0.4);

        let tyre = &config.tire_config;
        assert_eq!(tyre.optimal_pressure_kpa, 179.0);
        assert_eq!(tyre.pressure_front_kpa, 182.0);
        // Unnamed, a running pressure is the optimum.
        assert_eq!(tyre.pressure_rear_kpa, 179.0);
        // The shared key, and an axle's own key over it.
        assert_eq!(tyre.load_sensitivity_front, 0.85);
        assert_eq!(tyre.load_sensitivity_rear, 0.9);
        assert_eq!(tyre.reference_load_front_n, Some(3768.0));
        assert_eq!(tyre.reference_load_rear_n, Some(3768.0));
        assert_eq!(tyre.longitudinal_grip_factor, 1.1);
        assert_eq!(tyre.front_grip_scale, 0.98);
        assert_eq!(tyre.rear_grip_scale, 1.02);

        let turbo = config.engine.turbo.expect("turbo table");
        assert_eq!(turbo.boosted_share, 0.35);
        assert_eq!(turbo.lag_up_s, 0.6);
        assert_eq!(turbo.lag_down_s, DEFAULT_TURBO_LAG_DOWN_S);
    }

    #[test]
    fn without_the_tables_the_tyre_drivetrain_and_turbo_are_as_before() {
        let config = load_toml_str(BASE_TOML).expect("minimal car should load");
        let tyre = &config.tire_config;
        assert_eq!(tyre.pressure_front_kpa, 180.0);
        assert_eq!(tyre.pressure_rear_kpa, 180.0);
        assert_eq!(tyre.optimal_pressure_kpa, 180.0);
        assert_eq!(tyre.load_sensitivity_front, 1.0);
        assert_eq!(tyre.load_sensitivity_rear, 1.0);
        assert_eq!(tyre.reference_load_front_n, None);
        assert_eq!(tyre.reference_load_rear_n, None);
        assert_eq!(tyre.longitudinal_grip_factor, 1.0);
        assert_eq!(tyre.front_grip_scale, 1.0);
        assert_eq!(tyre.rear_grip_scale, 1.0);
        assert!(tyre.is_linear_single_compound());
        assert_eq!(config.awd_front_share, DEFAULT_AWD_FRONT_SHARE);
        assert!(!config.differential.simulated);
        assert!(config.engine.turbo.is_none());
    }

    /// A car.toml that spells out every new key at its default drives to
    /// the bit like one that has none of them.
    #[test]
    fn default_valued_tables_simulate_like_no_tables() {
        let with_layout =
            |extra: &str| format!("{BASE_TOML}\n[drivetrain]\nlayout = \"AWD\"\n{extra}");
        let bare = load_toml_str(&with_layout("")).unwrap();
        let explicit = load_toml_str(&with_layout(
            r#"awd_front_share = 0.4

[differential]
simulated = false

[tires]
optimal_pressure_kpa = 180.0
pressure_front_kpa = 180.0
pressure_rear_kpa = 180.0
load_sensitivity = 1.0
reference_load_n = 4000.0
longitudinal_grip_factor = 1.0
front_grip_scale = 1.0
rear_grip_scale = 1.0

[engine.turbo]
boosted_share = 0.0
"#,
        ))
        .unwrap();

        let run = |config: &CarConfig| {
            let track = TrackConfig {
                centerline: (0..600)
                    .map(|i| TrackPoint {
                        x: i as f32 * 4.0,
                        distance_from_start_m: i as f32 * 4.0,
                        width_left_m: 500.0,
                        width_right_m: 500.0,
                        ..Default::default()
                    })
                    .collect(),
                ..TrackConfig::default()
            };
            let slot = GridSlot {
                position: 1,
                x: 0.0,
                y: 0.0,
                z: 0.0,
                yaw_rad: 0.0,
            };
            let mut state = CarState::new(Uuid::from_u128(1), config.id, &slot);
            state.auto_gearbox = true;
            state.traction_control = Some(TractionControl::Off);
            let mut bits = Vec::new();
            for tick in 0..240 * 8u32 {
                let t = tick as f32 / 240.0;
                let braking = (5.0..6.0).contains(&t);
                let input = PlayerInputData {
                    throttle: if braking { 0.0 } else { 1.0 },
                    brake: if braking { 1.0 } else { 0.0 },
                    steering: 0.5 * (t * 1.1).sin(),
                    ..Default::default()
                };
                crate::physics::update_car_3d(&mut state, config, &input, &track, 1.0 / 240.0);
                bits.extend(
                    [
                        state.pos_x,
                        state.pos_y,
                        state.yaw_rad,
                        state.vel_x,
                        state.engine_rpm,
                    ]
                    .map(f32::to_bits),
                );
            }
            bits
        };
        assert_eq!(run(&bare), run(&explicit));
    }

    #[test]
    fn out_of_range_tyre_drivetrain_and_turbo_keys_are_rejected() {
        for bad in [
            "[tires]\nload_sensitivity = 1.5\n",
            "[tires]\nload_sensitivity_front = 0.1\n",
            "[tires]\nreference_load_n = 10.0\n",
            "[tires]\nlongitudinal_grip_factor = 3.0\n",
            "[tires]\nrear_grip_scale = 0.2\n",
            "[tires]\noptimal_pressure_kpa = 20.0\n",
            "[tires]\npressure_rear_kpa = 900.0\n",
            "[tires]\ncamber = 1.0\n",
            "[drivetrain]\nawd_front_share = 1.2\n",
            "[differential]\nlock_power = 1.5\n",
            "[differential]\npreload_nm = -10.0\n",
            "[engine.turbo]\nboosted_share = 0.95\n",
            "[engine.turbo]\nboosted_share = 0.3\nlag_up_s = -1.0\n",
        ] {
            assert!(
                load_toml_str(&format!("{BASE_TOML}\n{bad}")).is_err(),
                "should be refused: {bad}"
            );
        }
    }

    /// The garage's pressure clicks move the car's own running pressures,
    /// so a car whose optimum is not 180 kPa is still at its optimum stock
    /// and loses grip either side of it.
    #[test]
    fn the_garage_clicks_pressure_from_the_files_own_optimum() {
        let config = load_toml_str(&format!(
            "{BASE_TOML}\n[tires]\noptimal_pressure_kpa = 165.0\n"
        ))
        .unwrap();
        assert_eq!(config.tire_config.front_grip_factor(), 1.0);
        let setup = crate::car_setup::CarSetup {
            tyre_pressure_front: 2,
            ..Default::default()
        };
        let tuned = setup.apply(&config);
        assert_eq!(tuned.tire_config.pressure_front_kpa, 175.0);
        assert_eq!(tuned.tire_config.pressure_rear_kpa, 165.0);
        assert!(tuned.tire_config.front_grip_factor() < 1.0);
        assert_eq!(tuned.tire_config.rear_grip_factor(), 1.0);
    }
}
