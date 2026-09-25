use serde::{Deserialize, Serialize};
use serde_repr::{Deserialize_repr, Serialize_repr};
use uuid::Uuid;

// --- Identifiers ---
pub type PlayerId = Uuid;
pub type SessionId = Uuid;
pub type CarConfigId = Uuid;
pub type TrackConfigId = Uuid;
pub type ConnectionId = Uuid;

// --- Player State ---
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Player {
    pub id: PlayerId,
    pub name: String,
    pub connection_id: ConnectionId,
    pub selected_car_config_id: Option<CarConfigId>,
    pub is_ai: bool,
}

// --- Car Configuration (Static / Moddable) ---
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct CarConfig {
    pub id: CarConfigId,
    pub name: String,
    pub model: String,
    /// Racing class from `car.toml` (`GT3`, `LMP2`, `F1`, ...), empty when the
    /// file names none. AI fields are drawn from the host car's class.
    #[serde(default)]
    pub class: String,
    /// Checksum of the `car.toml` this was loaded from (`content_crc`), 0 for
    /// a config that never came from a file. Sent to clients so they can tell
    /// whether their imported car matches.
    #[serde(default)]
    pub content_crc: u32,
    /// Names of the car's extra liveries, the `[[livery]]` tables of its
    /// `car.toml` in order. Livery 0 is the model as authored; 1..=len are
    /// these. The server only relays which one a driver picked; the colours
    /// and logos live on the client's catalog row.
    #[serde(default)]
    pub livery_names: Vec<String>,

    // Physical dimensions
    pub mass_kg: f32,
    pub length_m: f32,
    pub width_m: f32,
    pub height_m: f32,
    pub wheelbase_m: f32,
    pub track_width_front_m: f32, // Distance between front wheels
    pub track_width_rear_m: f32,  // Distance between rear wheels
    pub wheel_radius_m: f32,

    // Center of gravity (relative to geometric center)
    pub cog_height_m: f32,              // Height of center of gravity
    pub cog_offset_x_m: f32,            // Forward (+) / backward (-) from center
    pub weight_distribution_front: f32, // 0.0-1.0, percentage of weight on front axle

    // Engine & drivetrain
    pub max_engine_power_w: f32,   // Peak engine power in Watts
    pub max_engine_torque_nm: f32, // Peak engine torque in Nm
    pub max_engine_rpm: f32,
    pub idle_rpm: f32,
    pub redline_rpm: f32,
    pub gear_ratios: Vec<f32>, // Gear ratios (including reverse as negative)
    pub final_drive_ratio: f32,
    pub drivetrain: Drivetrain, // FWD, RWD, AWD

    // Engine simulation parameters (more detailed than the legacy fields above).
    // These are optional/soft-configurable: physics falls back to legacy behavior if unset.
    #[serde(default)]
    pub engine: EngineConfig,
    #[serde(default)]
    pub transmission: TransmissionConfig,
    #[serde(default)]
    pub differential: DifferentialConfig,
    #[serde(default)]
    pub fuel: FuelConfig,
    #[serde(default)]
    pub hybrid: HybridConfig,

    // Braking / driver aids
    pub max_brake_force_n: f32,
    pub brake_bias_front: f32, // 0.0-1.0, percentage of braking on front
    pub abs_enabled: bool,
    /// Traction control: caps drive torque at the traction limit instead of
    /// letting excess torque spin the wheels into the grip falloff.
    #[serde(default = "default_traction_control")]
    pub traction_control_enabled: bool,

    // Aerodynamics
    pub drag_coefficient: f32,
    pub frontal_area_m2: f32,
    pub lift_coefficient_front: f32, // Negative = downforce
    pub lift_coefficient_rear: f32,
    /// The drag reduction system, for a car that has one: what opening the
    /// rear wing's flap takes off the drag and the rear downforce. `None`
    /// for a car without a movable wing, which never opens one whatever
    /// the track offers.
    #[serde(default)]
    pub drs: Option<DrsSpec>,

    // Steering
    pub max_steering_angle_rad: f32,
    pub steering_ratio: f32, // Steering wheel turns : wheel angle

    // Suspension
    pub suspension: SuspensionConfig,

    // Tires
    pub tire_config: TireConfig,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, Default)]
pub enum Drivetrain {
    FWD, // Front-wheel drive
    #[default]
    RWD, // Rear-wheel drive
    AWD, // All-wheel drive
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, Default)]
pub enum TransmissionType {
    Manual,
    DCT,
    #[default]
    Sequential,
    Automatic,
    CVT,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, Default)]
pub enum DifferentialType {
    Open,
    Locked,
    #[default]
    ClutchLSD,
    ViscousLSD,
    Torsen,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize)]
pub struct TorqueCurvePoint {
    pub rpm: f32,
    pub torque_nm: f32,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct EngineConfig {
    /// RPM at which the limiter starts cutting torque.
    pub rev_limiter_rpm: f32,
    /// Optional torque curve used by physics when non-empty.
    pub torque_curve: Vec<TorqueCurvePoint>,
    /// Approx. engine rotational inertia.
    pub inertia_kg_m2: f32,
    /// Base friction torque opposing rotation.
    pub friction_torque_nm: f32,
    /// Additional negative torque when off-throttle (engine braking).
    pub engine_brake_torque_nm: f32,
    /// Idle controller strength (simple proportional gain).
    pub idle_control_gain: f32,
}

impl Default for EngineConfig {
    fn default() -> Self {
        Self {
            rev_limiter_rpm: 8000.0,
            torque_curve: Vec::new(),
            inertia_kg_m2: 0.25,
            friction_torque_nm: 20.0,
            engine_brake_torque_nm: 80.0,
            idle_control_gain: 0.15,
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TransmissionConfig {
    pub transmission_type: TransmissionType,
    /// Shift time used by automatic shifting / latency simulation.
    pub shift_time_s: f32,
    /// Drivetrain efficiency multiplier (0-1). Applied to wheel torque.
    pub efficiency: f32,
}

impl Default for TransmissionConfig {
    fn default() -> Self {
        Self {
            transmission_type: TransmissionType::Sequential,
            shift_time_s: 0.12,
            efficiency: 0.92,
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DifferentialConfig {
    pub differential_type: DifferentialType,
    /// Base preload torque for clutch LSD.
    pub preload_nm: f32,
    /// Lock factor on power (0-1).
    pub lock_power: f32,
    /// Lock factor on coast (0-1).
    pub lock_coast: f32,
}

impl Default for DifferentialConfig {
    fn default() -> Self {
        Self {
            differential_type: DifferentialType::ClutchLSD,
            preload_nm: 60.0,
            lock_power: 0.35,
            lock_coast: 0.20,
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct FuelConfig {
    pub capacity_liters: f32,
    /// Base consumption at idle (L/s)
    pub idle_consumption_lps: f32,
    /// Consumption scaling with throttle/rpm (tuned constant).
    pub load_consumption_scale: f32,
}

impl Default for FuelConfig {
    fn default() -> Self {
        Self {
            capacity_liters: 100.0,
            idle_consumption_lps: 0.00005,
            load_consumption_scale: 0.003,
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct HybridConfig {
    pub enabled: bool,
    pub battery_capacity_kwh: f32,
    pub battery_max_discharge_kw: f32,
    pub battery_max_charge_kw: f32,
    pub motor_max_torque_nm: f32,
    pub motor_max_power_kw: f32,
    pub regen_max_power_kw: f32,
}

impl Default for HybridConfig {
    fn default() -> Self {
        Self {
            enabled: false,
            battery_capacity_kwh: 0.0,
            battery_max_discharge_kw: 0.0,
            battery_max_charge_kw: 0.0,
            motor_max_torque_nm: 0.0,
            motor_max_power_kw: 0.0,
            regen_max_power_kw: 0.0,
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SuspensionConfig {
    pub spring_rate_front_n_per_m: f32,
    pub spring_rate_rear_n_per_m: f32,
    pub damper_compression_front: f32,
    pub damper_compression_rear: f32,
    pub damper_rebound_front: f32,
    pub damper_rebound_rear: f32,
    pub anti_roll_bar_front: f32,
    pub anti_roll_bar_rear: f32,
    pub max_travel_m: f32,
}

impl Default for SuspensionConfig {
    fn default() -> Self {
        Self {
            spring_rate_front_n_per_m: 80000.0,
            spring_rate_rear_n_per_m: 70000.0,
            damper_compression_front: 3000.0,
            damper_compression_rear: 2800.0,
            damper_rebound_front: 4500.0,
            damper_rebound_rear: 4200.0,
            anti_roll_bar_front: 15000.0,
            anti_roll_bar_rear: 12000.0,
            max_travel_m: 0.15,
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TireConfig {
    pub grip_coefficient: f32,         // Base grip coefficient (0.8-1.2)
    pub optimal_slip_ratio: f32,       // Peak longitudinal slip (typically 0.06-0.12)
    pub optimal_slip_angle_rad: f32,   // Peak lateral slip angle (typically 6-10 degrees)
    pub rolling_resistance: f32,       // Rolling resistance coefficient
    pub optimal_temperature_c: f32,    // Optimal tire temp for best grip
    pub temperature_grip_falloff: f32, // Grip reduction per degree from optimal
    pub wear_rate: f32,                // Wear rate multiplier
    /// Running pressure per axle and the pressure the tyre grips best at.
    /// Off the optimum the contact patch shrinks (or crowns) and the axle
    /// loses grip quadratically (`pressure_grip_factor`); the garage setup
    /// moves the running pressures, which is how it shifts the balance.
    #[serde(default = "default_tyre_pressure_kpa")]
    pub pressure_front_kpa: f32,
    #[serde(default = "default_tyre_pressure_kpa")]
    pub pressure_rear_kpa: f32,
    #[serde(default = "default_tyre_pressure_kpa")]
    pub optimal_pressure_kpa: f32,
}

fn default_tyre_pressure_kpa() -> f32 {
    180.0
}

/// Grip lost per unit of (pressure error / optimum) squared: 25 kPa off a
/// 180 kPa optimum (five clicks) costs the axle 4%.
pub const TYRE_PRESSURE_GRIP_LOSS: f32 = 2.07;

impl TireConfig {
    /// Grip multiplier for an axle at `pressure_kpa`: exactly 1.0 at the
    /// optimum (so a stock car is bit-identical to before the field), less
    /// either side, never below 0.5.
    pub fn pressure_grip_factor(&self, pressure_kpa: f32) -> f32 {
        let optimum = self.optimal_pressure_kpa.max(1.0);
        let error = (pressure_kpa - optimum) / optimum;
        if error == 0.0 {
            return 1.0;
        }
        (1.0 - TYRE_PRESSURE_GRIP_LOSS * error * error).max(0.5)
    }

    pub fn front_grip_factor(&self) -> f32 {
        self.pressure_grip_factor(self.pressure_front_kpa)
    }

    pub fn rear_grip_factor(&self) -> f32 {
        self.pressure_grip_factor(self.pressure_rear_kpa)
    }
}

impl Default for TireConfig {
    fn default() -> Self {
        Self {
            grip_coefficient: 1.0,
            optimal_slip_ratio: 0.08,
            optimal_slip_angle_rad: 0.12, // ~7 degrees
            rolling_resistance: 0.015,
            optimal_temperature_c: 90.0,
            temperature_grip_falloff: 0.005,
            wear_rate: 1.0,
            pressure_front_kpa: 180.0,
            pressure_rear_kpa: 180.0,
            optimal_pressure_kpa: 180.0,
        }
    }
}

impl Default for CarConfig {
    fn default() -> Self {
        Self {
            id: Uuid::new_v4(),
            name: "Default Car".to_string(),
            model: "default.glb".to_string(),
            class: String::new(),
            content_crc: 0,
            livery_names: Vec::new(),

            // Physical dimensions
            mass_kg: 1200.0,
            length_m: 4.5,
            width_m: 1.9,
            height_m: 1.3,
            wheelbase_m: 2.7,
            track_width_front_m: 1.6,
            track_width_rear_m: 1.58,
            wheel_radius_m: 0.33,

            // Center of gravity
            cog_height_m: 0.45,
            cog_offset_x_m: 0.0,
            weight_distribution_front: 0.52,

            // Engine & drivetrain
            max_engine_power_w: 300000.0, // 300 kW (~400 HP)
            max_engine_torque_nm: 450.0,
            max_engine_rpm: 8000.0,
            idle_rpm: 900.0,
            redline_rpm: 7500.0,
            gear_ratios: vec![-3.5, 3.8, 2.4, 1.7, 1.3, 1.0, 0.8], // R, 1-6
            final_drive_ratio: 3.7,
            drivetrain: Drivetrain::RWD,

            engine: EngineConfig {
                rev_limiter_rpm: 8000.0,
                ..EngineConfig::default()
            },
            transmission: TransmissionConfig::default(),
            differential: DifferentialConfig::default(),
            fuel: FuelConfig::default(),
            hybrid: HybridConfig::default(),

            // Braking / driver aids
            max_brake_force_n: 25000.0,
            brake_bias_front: 0.6,
            abs_enabled: true,
            traction_control_enabled: true,

            // Aerodynamics
            drag_coefficient: 0.32,
            frontal_area_m2: 2.2,
            lift_coefficient_front: -0.15, // Slight downforce
            lift_coefficient_rear: -0.20,
            drs: None,

            // Steering
            max_steering_angle_rad: 0.52, // ~30 degrees
            steering_ratio: 14.0,

            // Suspension
            suspension: SuspensionConfig::default(),

            // Tires
            tire_config: TireConfig::default(),
        }
    }
}

// --- Track Configuration (Static / Moddable) ---
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TrackConfig {
    pub id: TrackConfigId,
    pub name: String,
    pub centerline: Vec<TrackPoint>,
    pub width_m: f32,
    /// Path to the source track file, relative to the content folder (e.g. "tracks/real/Austin.yaml")
    #[serde(default)]
    pub source_path: Option<String>,
    /// Checksum of the track file this was loaded from (`content_crc`), 0
    /// for a track built in memory. Sent to clients so they can tell whether
    /// their baked level matches.
    #[serde(default)]
    pub content_crc: u32,
    pub start_positions: Vec<GridSlot>,
    pub track_surface: TrackSurface,
    pub pit_lane: Option<PitLaneConfig>,
    /// Optional optimal racing line for AI and visualization
    #[serde(default)]
    pub raceline: Vec<RacelinePoint>,
    /// The DRS zones, from the track file's `drs_zones`. Empty for a
    /// circuit that has none.
    #[serde(default)]
    pub drs_zones: Vec<DrsZone>,
    /// Cumulative arc-length (m) of each raceline point, computed at load
    /// time. Runtime-only lookup data (parallel to `raceline`); never
    /// serialized. Rebuild with [`TrackConfig::rebuild_raceline_distances`].
    #[serde(skip)]
    pub raceline_distances: Vec<f32>,
    /// Checkpoint distances along the centerline (m from start), sorted
    /// ascending. Used for anti-shortcut lap validation. Runtime-only data
    /// computed at load time from the track file's checkpoint node indices;
    /// when empty, physics falls back to virtual checkpoints at 25/50/75%.
    #[serde(skip)]
    pub checkpoints: Vec<f32>,
    /// Sector boundary distances along the centerline (m from start), one
    /// short of [`crate::laps::SECTOR_COUNT`] and ascending. Runtime-only,
    /// resolved at load time from the track file's `sectors` node indices;
    /// when empty the lap is split into even thirds.
    #[serde(skip)]
    pub sectors: Vec<f32>,
    /// Track metadata
    #[serde(default)]
    pub metadata: TrackMetadata,
    /// Optional procedural world data
    #[serde(default)]
    pub procedural_world: Option<crate::procgen::ProceduralWorldData>,
    /// Baked ground heightfield exported by the track editor alongside the
    /// Unreal scene (`<Track>.ground.msgpack`). Off the asphalt the sim
    /// follows this instead of the centerline elevation, so the car rests on
    /// the ground the client renders. Runtime-only; loaded from the sidecar.
    #[serde(skip)]
    pub ground: Option<crate::ground::GroundHeightfield>,
    /// Baked curb widths exported alongside the Unreal scene
    /// (`<Track>.curbs.msgpack`). How far the curbs reach past each road
    /// edge, so a car using them counts as on the track. Runtime-only;
    /// loaded from the sidecar.
    #[serde(skip)]
    pub curbs: Option<crate::curbs::CurbBands>,
    /// Baked walls exported alongside the Unreal scene
    /// (`<Track>.walls.msgpack`): the barriers, tire walls, pit walls,
    /// stands and buildings as solid faces, so a car that leaves the road
    /// is stopped by them rather than driving through. Runtime-only;
    /// loaded from the sidecar.
    #[serde(skip)]
    pub walls: Option<crate::walls::Walls>,
}

/// What a car's DRS does when open: fractions of the drag and of the rear
/// downforce that the open flap takes away.
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
pub struct DrsSpec {
    pub drag_reduction: f32,
    pub rear_downforce_reduction: f32,
}

impl DrsSpec {
    /// A modern F1 rear wing: about a tenth of the car's drag and a
    /// quarter of the rear downforce.
    pub const F1: DrsSpec = DrsSpec {
        drag_reduction: 0.12,
        rear_downforce_reduction: 0.25,
    };
}

/// One DRS zone along the centerline, stations in metres from the start
/// line: the gap to the car ahead is measured as a car crosses
/// `detection_m`, the flap may open from `start_m` and must be shut by
/// `end_m`. A zone may wrap through the start line.
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
pub struct DrsZone {
    pub detection_m: f32,
    pub start_m: f32,
    pub end_m: f32,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct RacelinePoint {
    pub x: f32,
    pub y: f32,
    pub z: f32,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct TrackMetadata {
    pub country: Option<String>,
    pub city: Option<String>,
    pub length_m: Option<f32>,
    pub description: Option<String>,
    pub year_built: Option<u32>,
    pub category: Option<String>, // e.g., "F1", "DTM", "IndyCar"

    // Procedural generation parameters
    #[serde(default)]
    pub environment_type: Option<String>, // "desert" | "forest" | "city" | "mountains" | "plains"
    #[serde(default)]
    pub terrain_seed: Option<u32>, // Deterministic seed (None = generate from track ID)
    #[serde(default)]
    pub terrain_scale: Option<f32>, // Height multiplier (default: 1.0)
    #[serde(default)]
    pub terrain_detail: Option<f32>, // Noise frequency multiplier (default: 0.5)
    #[serde(default)]
    pub terrain_blend_width: Option<f32>, // Track corridor blend meters (default: 20.0)
    #[serde(default)]
    pub object_density: Option<f32>, // 0-1 density multiplier (default: 0.8)
    #[serde(default)]
    pub decal_profile: Option<String>, // Decal set identifier (default: "default")
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TrackSurface {
    pub base_grip: f32,      // Base grip multiplier (1.0 = normal asphalt)
    pub curb_grip: f32,      // Grip on curbs
    pub off_track_grip: f32, // Grip off track (grass/gravel)
    /// Rolling drag off track, m/s², opposing the car's motion. A fixed
    /// deceleration rather than a fraction of speed per second: grass is
    /// already slow through its grip, and a proportional drag fought the
    /// throttle so hard that a car could not get back above a crawl.
    pub off_track_drag_mps2: f32,
}

/// Rolling drag on grass: about 0.06 g, a car tyre's rolling resistance on
/// turf. Kept well under what grass grip can put down (about 2 m/s² for a
/// rear-driven car), or the car still cannot pull away.
pub const OFF_TRACK_DRAG_MPS2: f32 = 0.6;

impl Default for TrackSurface {
    fn default() -> Self {
        Self {
            base_grip: 1.0,
            curb_grip: 0.85,
            off_track_grip: 0.4,
            off_track_drag_mps2: OFF_TRACK_DRAG_MPS2,
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PitLaneConfig {
    pub entry_point: TrackPoint,
    pub exit_point: TrackPoint,
    pub speed_limit_mps: f32,
    pub pit_stalls: Vec<PitStall>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PitStall {
    pub position: u8,
    pub x: f32,
    pub y: f32,
    pub z: f32,
}

impl Default for TrackConfig {
    fn default() -> Self {
        // Create a simple oval track with elevation changes
        let mut centerline = Vec::new();
        let num_points = 40;
        let radius = 100.0;

        for i in 0..num_points {
            let angle = 2.0 * std::f32::consts::PI * (i as f32) / (num_points as f32);
            let x = radius * angle.cos();
            let y = radius * angle.sin();
            let distance = angle * radius;

            // Add some elevation variation
            let z = (angle * 2.0).sin() * 3.0; // ±3m elevation

            // Calculate banking based on turn (more banking in turns)
            let banking = if x.abs() < radius * 0.3 {
                0.12 * (1.0 - x.abs() / (radius * 0.3)) // ~7 degrees max
            } else {
                0.0
            };

            // Calculate track direction for camber
            let next_i = (i + 1) % num_points;
            let next_angle = 2.0 * std::f32::consts::PI * (next_i as f32) / (num_points as f32);
            let dx = (radius * next_angle.cos()) - x;
            let dy = (radius * next_angle.sin()) - y;
            let heading = dy.atan2(dx);

            // Calculate slope (grade) based on elevation change
            let prev_i = (i + num_points - 1) % num_points;
            let prev_angle = 2.0 * std::f32::consts::PI * (prev_i as f32) / (num_points as f32);
            let prev_z = (prev_angle * 2.0).sin() * 3.0;
            let segment_length = 2.0 * std::f32::consts::PI * radius / num_points as f32;
            let slope = (z - prev_z) / segment_length;

            centerline.push(TrackPoint {
                x,
                y,
                z,
                distance_from_start_m: distance,
                width_left_m: 7.5,
                width_right_m: 7.5,
                banking_rad: banking,
                camber_rad: 0.0,
                slope_rad: slope.atan(),
                heading_rad: heading,
                surface_type: SurfaceType::Asphalt,
                grip_modifier: 1.0,
            });
        }

        // Create start positions
        let mut start_positions = Vec::new();
        for i in 0..16 {
            start_positions.push(GridSlot {
                position: (i + 1) as u8,
                x: radius - (i / 2) as f32 * 8.0,
                y: if i % 2 == 0 { -2.0 } else { 2.0 },
                z: 0.0,
                yaw_rad: 0.0,
            });
        }

        Self {
            id: Uuid::new_v4(),
            name: "Default Oval".to_string(),
            centerline,
            width_m: 15.0,
            source_path: None,
            content_crc: 0,
            start_positions,
            track_surface: TrackSurface::default(),
            pit_lane: None,
            raceline: Vec::new(),
            drs_zones: Vec::new(),
            raceline_distances: Vec::new(),
            checkpoints: Vec::new(),
            sectors: Vec::new(),
            metadata: TrackMetadata::default(),
            procedural_world: None,
            ground: None,
            curbs: None,
            walls: None,
        }
    }
}

impl TrackConfig {
    /// Recompute `raceline_distances` (cumulative arc length per raceline
    /// point). Must be called whenever `raceline` is (re)assigned outside the
    /// track loader; AI raceline-following falls back to the centerline when
    /// the distances are missing or out of sync.
    pub fn rebuild_raceline_distances(&mut self) {
        self.raceline_distances.clear();
        if self.raceline.is_empty() {
            return;
        }
        self.raceline_distances.reserve(self.raceline.len());
        let mut cumulative = 0.0f32;
        self.raceline_distances.push(0.0);
        for i in 1..self.raceline.len() {
            let dx = self.raceline[i].x - self.raceline[i - 1].x;
            let dy = self.raceline[i].y - self.raceline[i - 1].y;
            let dz = self.raceline[i].z - self.raceline[i - 1].z;
            cumulative += (dx * dx + dy * dy + dz * dz).sqrt();
            self.raceline_distances.push(cumulative);
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize, Default)]
pub enum SurfaceType {
    #[default]
    Asphalt,
    Concrete,
    Curb,
    Grass,
    Gravel,
    Sand,
    Wet,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TrackPoint {
    pub x: f32,
    pub y: f32,
    pub z: f32, // Elevation
    pub distance_from_start_m: f32,
    pub width_left_m: f32,  // Track width to the left of centerline
    pub width_right_m: f32, // Track width to the right of centerline
    pub banking_rad: f32,   // Track banking angle (positive = banked towards inside)
    pub camber_rad: f32,    // Cross-slope (crown)
    pub slope_rad: f32,     // Uphill/downhill grade
    pub heading_rad: f32,   // Track direction at this point
    pub surface_type: SurfaceType,
    pub grip_modifier: f32, // Local grip adjustment (1.0 = normal)
}

impl Default for TrackPoint {
    fn default() -> Self {
        Self {
            x: 0.0,
            y: 0.0,
            z: 0.0,
            distance_from_start_m: 0.0,
            width_left_m: 7.5,
            width_right_m: 7.5,
            banking_rad: 0.0,
            camber_rad: 0.0,
            slope_rad: 0.0,
            heading_rad: 0.0,
            surface_type: SurfaceType::Asphalt,
            grip_modifier: 1.0,
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct GridSlot {
    pub position: u8,
    pub x: f32,
    pub y: f32,
    pub z: f32,
    pub yaw_rad: f32,
}

// --- Telemetry Data Structures ---
#[derive(Debug, Clone, Copy, Default, Serialize, Deserialize)]
pub struct TireData {
    pub temperature_c: f32,
    pub pressure_kpa: f32,
    pub wear_percent: f32,
    pub slip_ratio: f32,
    pub slip_angle_rad: f32,
}

#[derive(Debug, Clone, Copy, Default, Serialize, Deserialize)]
pub struct TireTelemetry {
    pub front_left: TireData,
    pub front_right: TireData,
    pub rear_left: TireData,
    pub rear_right: TireData,
}

#[derive(Debug, Clone, Copy, Default, Serialize, Deserialize)]
pub struct GForces {
    pub lateral_g: f32,      // Side-to-side (left negative, right positive)
    pub longitudinal_g: f32, // Forward/backward (braking negative, acceleration positive)
    pub vertical_g: f32,     // Up/down (compression positive)
}

#[derive(Debug, Clone, Copy, Default, Serialize, Deserialize)]
pub struct SuspensionTelemetry {
    pub front_left_travel_m: f32,
    pub front_right_travel_m: f32,
    pub rear_left_travel_m: f32,
    pub rear_right_travel_m: f32,
    pub front_left_velocity_mps: f32,
    pub front_right_velocity_mps: f32,
    pub rear_left_velocity_mps: f32,
    pub rear_right_velocity_mps: f32,
    pub front_left_spring_force_n: f32,
    pub front_right_spring_force_n: f32,
    pub rear_left_spring_force_n: f32,
    pub rear_right_spring_force_n: f32,
    pub front_left_damper_force_n: f32,
    pub front_right_damper_force_n: f32,
    pub rear_left_damper_force_n: f32,
    pub rear_right_damper_force_n: f32,
}

#[derive(Debug, Clone, Copy, Default, Serialize, Deserialize)]
pub struct DamageState {
    pub front_damage_percent: f32,
    pub rear_damage_percent: f32,
    pub left_damage_percent: f32,
    pub right_damage_percent: f32,
    pub engine_damage_percent: f32,
    pub is_drivable: bool,
}

// --- Car Dynamics State (Per-Tick, Server Authoritative) ---
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct CarState {
    pub player_id: PlayerId,
    pub car_config_id: CarConfigId,
    pub grid_position: u8,
    /// Server-side automatic gearbox, set by `ClientMessage::SetDriverAids`.
    /// The client only ever sends manual shifts; the server picks gears from
    /// the car's rev range and ratios, which the wire protocol never tells
    /// the client.
    #[serde(default)]
    pub auto_gearbox: bool,
    /// Ticks until the automatic box may shift again.
    #[serde(default)]
    pub auto_shift_hold_ticks: u16,
    /// Ticks the automatic box has seen the car at a standstill with the
    /// brake held; at `physics::AUTO_REVERSE_HOLD_S` it engages reverse.
    #[serde(default)]
    pub auto_reverse_ticks: u16,
    /// Speed-sensitive steering, set by `ClientMessage::SetDriverAids`: full
    /// steering input asks for the tightest turn the car can hold at its
    /// speed rather than the rack's full lock (`physics::assisted_steering`).
    /// Server-side because it needs the car's grip, downforce and wheelbase.
    /// Never set for AI drivers.
    #[serde(default)]
    pub steering_assist: bool,
    /// ABS for this driver, set by `ClientMessage::SetDriverAids`; `None`
    /// (AI drivers, older clients) leaves the car's own `abs_enabled`.
    #[serde(default)]
    pub abs: Option<bool>,
    /// Traction control level for this driver, set by
    /// `ClientMessage::SetDriverAids`; `None` uses the car's own
    /// `traction_control_enabled` (as `Low`).
    #[serde(default)]
    pub traction_control: Option<TractionControl>,

    // 3D Position
    pub pos_x: f32,
    pub pos_y: f32,
    pub pos_z: f32, // Elevation

    // 3D Orientation (Euler angles)
    pub yaw_rad: f32,   // Heading (rotation around Z axis)
    pub pitch_rad: f32, // Nose up/down (rotation around Y axis)
    pub roll_rad: f32,  // Body roll (rotation around X axis)

    // 3D Velocity
    pub vel_x: f32,
    pub vel_y: f32,
    pub vel_z: f32,
    pub speed_mps: f32, // Magnitude of velocity vector

    // Angular velocities
    pub angular_vel_yaw: f32,   // Yaw rate (rad/s)
    pub angular_vel_pitch: f32, // Pitch rate (rad/s)
    pub angular_vel_roll: f32,  // Roll rate (rad/s)

    // Inputs
    pub throttle_input: f32,
    pub brake_input: f32,
    pub steering_input: f32,
    pub gear: i8,          // Current gear (-1 = reverse, 0 = neutral, 1-6+)
    pub clutch_input: f32, // Clutch engagement (0 = disengaged, 1 = engaged)

    // Track position
    pub track_progress: f32,
    pub lateral_offset_m: f32, // Distance from centerline (positive = right)
    pub current_lap: u16,
    pub finish_position: Option<u8>,
    pub lap_start_tick: u32,      // Tick when current lap started
    pub current_lap_time_ms: u32, // Current lap time in progress
    pub last_lap_time_ms: Option<u32>,
    pub best_lap_time_ms: Option<u32>,

    // Collision state
    pub is_colliding: bool,
    pub collision_normal_x: f32,
    pub collision_normal_y: f32,
    pub collision_normal_z: f32,

    /// Cached nearest-centerline index from the previous physics query.
    /// Pure runtime search hint — never serialized, safe to reset to None.
    #[serde(skip)]
    pub nearest_centerline_idx: Option<u32>,

    /// Index of the next checkpoint this car must pass for its lap to count
    /// (anti-shortcut validation). Runtime-only; never serialized.
    #[serde(skip)]
    pub next_checkpoint: u8,

    /// Sectors, splits and track-limit state for the lap in progress
    /// (`crate::laps`). Runtime-only; never serialized.
    #[serde(skip)]
    pub laps: crate::laps::LapTiming,

    /// All four wheels were off the track on the last physics tick — what
    /// `crate::laps::note_track_limits` counts. Runtime-only.
    #[serde(skip)]
    pub wheels_off_track: bool,

    /// The car is parked in the garage of a hotlap session: not simulated,
    /// not collided with, hidden by the client, waiting for its driver to
    /// finish tuning and go out (`GameSession::hotlap_relocate`). Telemetry
    /// carries it as bit 2 of `lap_flags`.
    #[serde(default)]
    pub in_garage: bool,

    /// The drag reduction system (`crate::drs`). `drs_allowed`: the car is
    /// in an activation zone it earned at the detection point, so the
    /// driver may open the flap; `drs_open`: it is open this tick, and the
    /// aero is reduced by the car's `DrsSpec`. Telemetry carries both as
    /// bits 3 and 4 of `lap_flags`.
    #[serde(default)]
    pub drs_allowed: bool,
    #[serde(default)]
    pub drs_open: bool,

    /// The headlights are on (`crate::headlights`), and the driver is
    /// flashing them. Telemetry carries both as bits 5 and 6 of
    /// `lap_flags`; the client lights the car from them.
    #[serde(default)]
    pub headlights: bool,
    #[serde(default)]
    pub headlight_flash: bool,
    /// One bit per zone the car earned at its detection point, cleared as
    /// the zone ends. Runtime-only.
    #[serde(skip)]
    pub drs_armed: u32,
    /// Where the car was last tick, for spotting a detection line crossed.
    #[serde(skip)]
    pub drs_last_progress: f32,

    // Surface state
    pub current_surface: SurfaceType,
    pub is_on_track: bool,
    pub is_airborne: bool,
    pub surface_grip_modifier: f32,

    // Telemetry
    pub tires: TireTelemetry,
    pub g_forces: GForces,
    pub suspension: SuspensionTelemetry,
    pub fuel_liters: f32,
    pub fuel_capacity_liters: f32,
    pub fuel_consumption_lps: f32,
    pub damage: DamageState,
    pub engine_rpm: f32,
    pub engine_temp_c: f32,
    pub oil_temp_c: f32,
    pub oil_pressure_kpa: f32,
    pub water_temp_c: f32,

    // Weight transfer
    pub weight_front_left_n: f32,
    pub weight_front_right_n: f32,
    pub weight_rear_left_n: f32,
    pub weight_rear_right_n: f32,

    /// Per-wheel angular velocity (rad/s), order FL/FR/RL/RR. Consistent
    /// with each wheel's slip solution (spun-up under wheelspin, zero when
    /// locked).
    #[serde(default)]
    pub wheel_angular_vel: [f32; 4],

    /// Hybrid battery state of charge in kWh. Negative = not yet
    /// initialized; physics seeds it from the car's `[hybrid]` config on
    /// first tick.
    #[serde(default = "default_battery_uninitialized")]
    pub hybrid_battery_kwh: f32,

    // Aerodynamics
    pub downforce_front_n: f32,
    pub downforce_rear_n: f32,
    pub drag_force_n: f32,

    /// What the driver should feel, collected each tick for the next
    /// `DriverFeedback` message. Output only: nothing in the sim reads it.
    #[serde(skip)]
    pub feedback: crate::feedback::FeedbackAccumulator,
}

impl CarState {
    /// The aids this car runs, as a `DriverAids`.
    pub fn driver_aids(&self) -> DriverAids {
        DriverAids {
            auto_gearbox: self.auto_gearbox,
            steering_assist: self.steering_assist,
            abs: self.abs,
            traction_control: self.traction_control,
        }
    }

    /// Set the aids; the automatic box starts its timers afresh.
    pub fn set_driver_aids(&mut self, aids: DriverAids) {
        self.auto_gearbox = aids.auto_gearbox;
        self.auto_shift_hold_ticks = 0;
        self.auto_reverse_ticks = 0;
        self.steering_assist = aids.steering_assist;
        self.abs = aids.abs;
        self.traction_control = aids.traction_control;
    }

    pub fn new(player_id: PlayerId, car_config_id: CarConfigId, grid_slot: &GridSlot) -> Self {
        Self {
            player_id,
            car_config_id,
            grid_position: grid_slot.position,
            auto_gearbox: false,
            auto_shift_hold_ticks: 0,
            auto_reverse_ticks: 0,
            steering_assist: false,
            abs: None,
            traction_control: None,

            // 3D Position
            pos_x: grid_slot.x,
            pos_y: grid_slot.y,
            pos_z: grid_slot.z,

            // Orientation
            yaw_rad: grid_slot.yaw_rad,
            pitch_rad: 0.0,
            roll_rad: 0.0,

            // Velocity
            vel_x: 0.0,
            vel_y: 0.0,
            vel_z: 0.0,
            speed_mps: 0.0,

            // Angular velocity
            angular_vel_yaw: 0.0,
            angular_vel_pitch: 0.0,
            angular_vel_roll: 0.0,

            // Inputs
            throttle_input: 0.0,
            brake_input: 0.0,
            steering_input: 0.0,
            gear: 1,
            clutch_input: 1.0,

            // Track position
            track_progress: 0.0,
            lateral_offset_m: 0.0,
            current_lap: 0,
            finish_position: None,
            lap_start_tick: 0,
            current_lap_time_ms: 0,
            last_lap_time_ms: None,
            best_lap_time_ms: None,

            // Collision
            is_colliding: false,
            nearest_centerline_idx: None,
            next_checkpoint: 0,
            laps: crate::laps::LapTiming::default(),
            wheels_off_track: false,
            in_garage: false,
            drs_allowed: false,
            drs_open: false,
            headlights: false,
            headlight_flash: false,
            drs_armed: 0,
            drs_last_progress: 0.0,
            collision_normal_x: 0.0,
            collision_normal_y: 0.0,
            collision_normal_z: 0.0,

            // Surface
            current_surface: SurfaceType::Asphalt,
            is_on_track: true,
            is_airborne: false,
            surface_grip_modifier: 1.0,

            // Telemetry
            tires: TireTelemetry::default(),
            g_forces: GForces::default(),
            suspension: SuspensionTelemetry::default(),
            fuel_liters: 100.0,
            fuel_capacity_liters: 100.0,
            fuel_consumption_lps: 0.0,
            damage: DamageState {
                is_drivable: true,
                ..Default::default()
            },
            engine_rpm: 900.0,
            engine_temp_c: 85.0,
            oil_temp_c: 90.0,
            oil_pressure_kpa: 350.0,
            water_temp_c: 80.0,

            // Weight (will be calculated)
            weight_front_left_n: 0.0,
            weight_front_right_n: 0.0,
            weight_rear_left_n: 0.0,
            weight_rear_right_n: 0.0,

            wheel_angular_vel: [0.0; 4],
            hybrid_battery_kwh: default_battery_uninitialized(),

            // Aerodynamics (will be calculated)
            downforce_front_n: 0.0,
            downforce_rear_n: 0.0,
            drag_force_n: 0.0,

            feedback: Default::default(),
        }
    }
}

fn default_battery_uninitialized() -> f32 {
    -1.0
}

fn default_traction_control() -> bool {
    true
}

// --- Race Session State (Server Authoritative) ---
#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize_repr, Deserialize_repr)]
pub enum SessionState {
    Lobby = 0,
    Countdown = 1,
    Racing = 2,
    Finished = 3,
}

#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize_repr, Deserialize_repr, Default)]
pub enum SessionKind {
    #[default]
    Multiplayer = 0,
    Practice = 1,
    Sandbox = 2,
    /// An AI-only race the creator watches: the backdrop behind a client's
    /// menu. Unlisted, unjoinable, spectated by its creator, counted straight
    /// into a race, and never recorded as a replay. It ends when its last
    /// spectator leaves.
    Demo = 3,
}

/// How hard the traction control intervenes, chosen per player
/// (`ClientMessage::SetDriverAids`). Encoded as a small integer like
/// `GameMode`, so the Unreal codec writes it the same way.
#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize_repr, Deserialize_repr, Default)]
pub enum TractionControl {
    /// Excess drive torque spins the wheels into the grip falloff.
    Off = 0,
    /// Cuts drive only when the driven wheel would spin: the tyre is held at
    /// its peak slip and gives its peak force, but the throttle can still
    /// take the whole friction circle and push the rear wide in a corner.
    #[default]
    Low = 1,
    /// Cuts drive as soon as the tyre's combined grip runs out, leaving a
    /// margin for the lateral force: full throttle out of a corner is
    /// trimmed to what the rear can carry while it is still cornering.
    High = 2,
}

/// The aids a `SetDriverAids` asks for. The session's `AllowedAssists`
/// are applied on top (`AllowedAssists::clamp`), so a player can ask for
/// whatever their settings say and the server keeps what the host allowed.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct DriverAids {
    pub auto_gearbox: bool,
    pub steering_assist: bool,
    /// `None` keeps the car's own `abs_enabled`.
    pub abs: Option<bool>,
    /// `None` keeps the car's own `traction_control_enabled` (as `Low`).
    pub traction_control: Option<TractionControl>,
}

/// Which driving aids a session's host lets its drivers use, fixed when the
/// session is created. A disallowed aid is forced off for every human in the
/// session whatever their settings say: the server is the only place the
/// aids run, so this is where the rule holds. Absent fields (an older
/// client) allow the aid, so a session from before the rule is unchanged.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub struct AllowedAssists {
    #[serde(default = "default_true")]
    pub abs: bool,
    #[serde(default = "default_true")]
    pub traction_control: bool,
    #[serde(default = "default_true")]
    pub auto_gearbox: bool,
    #[serde(default = "default_true")]
    pub steering_assist: bool,
    /// Whether the server sends the `RacingLine` on join at all; without it
    /// the client has nothing to draw.
    #[serde(default = "default_true")]
    pub racing_line: bool,
}

fn default_true() -> bool {
    true
}

impl Default for AllowedAssists {
    fn default() -> Self {
        Self::ALL
    }
}

impl AllowedAssists {
    /// Every aid allowed: what a session from an older client gets.
    pub const ALL: AllowedAssists = AllowedAssists {
        abs: true,
        traction_control: true,
        auto_gearbox: true,
        steering_assist: true,
        racing_line: true,
    };

    /// Nothing allowed but the driver.
    pub const NONE: AllowedAssists = AllowedAssists {
        abs: false,
        traction_control: false,
        auto_gearbox: false,
        steering_assist: false,
        racing_line: false,
    };

    /// The aids a driver may actually run here: what they asked for, less
    /// whatever the session forbids. A forbidden ABS or traction control is
    /// pinned to `Some(off)` rather than left `None`, or the car's own file
    /// would switch it back on.
    pub fn clamp(&self, asked: DriverAids) -> DriverAids {
        DriverAids {
            auto_gearbox: asked.auto_gearbox && self.auto_gearbox,
            steering_assist: asked.steering_assist && self.steering_assist,
            abs: if self.abs { asked.abs } else { Some(false) },
            traction_control: if self.traction_control {
                asked.traction_control
            } else {
                Some(TractionControl::Off)
            },
        }
    }
}

/// The sky over a session, chosen by its host when the session is created
/// and fixed for its life. The server owns the grip it costs
/// (`SessionConditions::apply_to_track`); the client draws it.
#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize_repr, Deserialize_repr, Default)]
pub enum Weather {
    #[default]
    Sunny = 0,
    Cloudy = 1,
    Overcast = 2,
    LightRain = 3,
    HeavyRain = 4,
}

impl Weather {
    pub const ALL: [Weather; 5] = [
        Weather::Sunny,
        Weather::Cloudy,
        Weather::Overcast,
        Weather::LightRain,
        Weather::HeavyRain,
    ];

    pub fn from_u8(value: u8) -> Option<Weather> {
        Weather::ALL.get(value as usize).copied()
    }

    pub fn is_wet(self) -> bool {
        matches!(self, Weather::LightRain | Weather::HeavyRain)
    }

    /// What the asphalt's grip is multiplied by. Dry weather leaves the
    /// track as filed; a cold overcast track is a shade slower; rain takes
    /// the field down to where a wet race sits, some 15% off a dry lap.
    pub fn road_grip_factor(self) -> f32 {
        match self {
            Weather::Sunny | Weather::Cloudy => 1.0,
            Weather::Overcast => 0.98,
            Weather::LightRain => 0.86,
            Weather::HeavyRain => 0.74,
        }
    }

    /// Curbs are painted, and paint in the wet is close to ice: a further
    /// cut on top of `road_grip_factor`.
    pub fn curb_grip_factor(self) -> f32 {
        match self {
            Weather::LightRain => 0.85,
            Weather::HeavyRain => 0.75,
            _ => 1.0,
        }
    }

    /// Wet grass and gravel, on top of `road_grip_factor`.
    pub fn off_track_grip_factor(self) -> f32 {
        match self {
            Weather::LightRain => 0.85,
            Weather::HeavyRain => 0.7,
            _ => 1.0,
        }
    }

    pub fn name(self) -> &'static str {
        match self {
            Weather::Sunny => "sunny",
            Weather::Cloudy => "cloudy",
            Weather::Overcast => "overcast",
            Weather::LightRain => "light rain",
            Weather::HeavyRain => "heavy rain",
        }
    }
}

/// Weather and the clock, per session. Carried inside `CreateSession`,
/// echoed in `SessionJoined` and listed in every `SessionSummary`. Absent
/// fields (a client or server from before the feature) mean a sunny early
/// afternoon, which is what every session was until now.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub struct SessionConditions {
    #[serde(default)]
    pub weather: Weather,
    /// Local time of day, minutes after midnight (0..=1439). Visual only:
    /// the sim does not know night from day, the client's sun does.
    #[serde(default = "SessionConditions::default_time_of_day")]
    pub time_of_day_minutes: u16,
}

impl Default for SessionConditions {
    fn default() -> Self {
        Self::DEFAULT
    }
}

impl SessionConditions {
    /// A sunny 13:00: what every session was before conditions existed.
    pub const DEFAULT: SessionConditions = SessionConditions {
        weather: Weather::Sunny,
        time_of_day_minutes: 13 * 60,
    };
    pub const MINUTES_PER_DAY: u16 = 24 * 60;

    fn default_time_of_day() -> u16 {
        Self::DEFAULT.time_of_day_minutes
    }

    /// The same conditions with the clock wrapped onto one day.
    pub fn clamp(self) -> Self {
        Self {
            weather: self.weather,
            time_of_day_minutes: self.time_of_day_minutes % Self::MINUTES_PER_DAY,
        }
    }

    /// Bakes the weather's grip into a session's own copy of the track:
    /// every centerline sample's grip, the surface's base figure (which the
    /// racing line and the AI's speed profile read) and the curb and
    /// off-track grips. Physics, the AI and the racing line then follow
    /// without a branch in the hot loop, and a dry session is the track to
    /// the bit.
    pub fn apply_to_track(&self, track: &mut TrackConfig) {
        let road = self.weather.road_grip_factor();
        if road == 1.0 && self.weather.curb_grip_factor() == 1.0 {
            return;
        }
        for point in &mut track.centerline {
            point.grip_modifier *= road;
        }
        track.track_surface.base_grip *= road;
        track.track_surface.curb_grip *= road * self.weather.curb_grip_factor();
        track.track_surface.off_track_grip *= road * self.weather.off_track_grip_factor();
    }

    /// The sun's elevation, degrees, at this session's clock: the client's
    /// sky model (`ApexSky::SunAt`) to the letter — one late-May day at 50°
    /// N, solar noon at 13:00 — so the server's idea of dusk is the one the
    /// player sees.
    pub fn sun_elevation_deg(&self) -> f32 {
        const LATITUDE_DEG: f32 = 50.0;
        const DECLINATION_DEG: f32 = 20.0;
        const SOLAR_NOON_HOURS: f32 = 13.0;
        let hours = (self.time_of_day_minutes % Self::MINUTES_PER_DAY) as f32 / 60.0;
        let lat = LATITUDE_DEG.to_radians();
        let dec = DECLINATION_DEG.to_radians();
        let hour_angle = ((hours - SOLAR_NOON_HOURS) * 15.0).to_radians();
        let sin_elev = lat.sin() * dec.sin() + lat.cos() * dec.cos() * hour_angle.cos();
        sin_elev.clamp(-1.0, 1.0).asin().to_degrees()
    }

    /// Whether a car's lights are on when nobody has touched the switch:
    /// as the sun goes (under 6°) and in the rain, the client's
    /// `FSkyState::bHeadlights`.
    pub fn headlights_needed(&self) -> bool {
        self.sun_elevation_deg() < 6.0 || self.weather.is_wet()
    }

    pub fn is_night(&self) -> bool {
        let hour = self.time_of_day_minutes / 60;
        !(6..20).contains(&hour)
    }
}

/// Game modes determine the behavior and rules during a session
#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize_repr, Deserialize_repr, Default)]
pub enum GameMode {
    /// Lobby state, no telemetry sent, players selecting cars
    #[default]
    Lobby = 0,
    /// Nothing moves, no telemetry, camera exploration only
    Sandbox = 1,
    /// Pre-race countdown, players frozen in pit lane
    Countdown = 2,
    /// Server drives a demo car along the racing line
    DemoLap = 3,
    /// Players drive freely, optional lap timing
    FreePractice = 4,
    /// Playback recorded telemetry (view-only)
    Replay = 5,
    /// Qualification mode (to be implemented)
    Qualification = 6,
    /// Race mode (to be implemented)
    Race = 7,
    /// Time attack: every driver starts in the garage (tuning, nothing
    /// simulated), goes out onto a run-up before the line for flying laps,
    /// and can come back in at any time (`ClientMessage::HotlapRelocate`).
    /// Cars in the garage are neither ticked nor collided with, so several
    /// drivers can hotlap side by side on one track.
    Hotlap = 8,
}

/// Where a hotlap driver asks to be put.
#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize_repr, Deserialize_repr, Default)]
pub enum HotlapDestination {
    /// Back to the garage: parked, frozen, out of everyone's way.
    #[default]
    Garage = 0,
    /// Onto the track, on the run-up before the start line.
    Track = 1,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RaceSession {
    pub id: SessionId,
    pub track_config_id: TrackConfigId,
    pub host_player_id: PlayerId,
    /// The car selected by the host when creating the session
    #[serde(default)]
    pub host_car_id: Option<CarConfigId>,
    #[serde(default)]
    pub session_kind: SessionKind,
    /// What the host lets drivers use; see `AllowedAssists`.
    #[serde(default)]
    pub allowed_assists: AllowedAssists,
    /// The weather and the clock; see `SessionConditions`.
    #[serde(default)]
    pub conditions: SessionConditions,
    pub state: SessionState,
    #[serde(default)]
    pub game_mode: GameMode,
    /// Keyed and iterated in PlayerId order (BTreeMap): physics and the
    /// order-dependent collision solver must be deterministic across runs.
    pub participants: std::collections::BTreeMap<PlayerId, CarState>,
    pub max_players: u8,
    pub ai_count: u8,
    pub lap_limit: u8,
    pub current_tick: u32,
    pub countdown_ticks_remaining: Option<u16>,
    /// Game mode to transition to when the countdown reaches zero.
    #[serde(default)]
    pub next_mode: Option<GameMode>,
    pub race_start_tick: Option<u32>,
    /// AI driver player IDs (references to profiles stored elsewhere)
    pub ai_player_ids: Vec<PlayerId>,
    /// Demo lap state (used in DemoLap mode)
    pub demo_lap_progress: Option<f32>,
}

impl RaceSession {
    pub fn new(
        host_player_id: PlayerId,
        track_config_id: TrackConfigId,
        session_kind: SessionKind,
        max_players: u8,
        ai_count: u8,
        lap_limit: u8,
    ) -> Self {
        Self {
            id: Uuid::new_v4(),
            track_config_id,
            host_player_id,
            session_kind,
            allowed_assists: AllowedAssists::ALL,
            conditions: SessionConditions::DEFAULT,
            state: SessionState::Lobby,
            game_mode: GameMode::Lobby,
            participants: std::collections::BTreeMap::new(),
            max_players,
            ai_count,
            lap_limit,
            current_tick: 0,
            countdown_ticks_remaining: None,
            next_mode: None,
            race_start_tick: None,
            ai_player_ids: Vec::new(),
            demo_lap_progress: None,
            host_car_id: None,
        }
    }
}

// --- Input Data ---
#[derive(Debug, Clone, Copy, Default, Serialize, Deserialize)]
pub struct PlayerInputData {
    pub throttle: f32,
    pub brake: f32,
    pub steering: f32,
    pub gear: Option<i8>,    // Desired gear (-1 = reverse, 0 = neutral, 1-6+)
    pub clutch: Option<f32>, // Clutch input (0.0-1.0)
    /// The driver is holding the DRS button. Only opens the flap where
    /// `CarState::drs_allowed` says it may.
    pub drs: bool,
    /// The headlight switch: `Some` is the driver's choice, `None` leaves
    /// the lights to the conditions (`SessionConditions::headlights_needed`),
    /// which is what the AI and a client from before the field get.
    pub headlights: Option<bool>,
    /// The flash button is held: the lights flick to full beam whatever the
    /// switch says.
    pub flash: bool,
}

impl PlayerInputData {
    pub fn clamp(&mut self) {
        self.throttle = self.throttle.clamp(0.0, 1.0);
        self.brake = self.brake.clamp(0.0, 1.0);
        self.steering = self.steering.clamp(-1.0, 1.0);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_player_creation() {
        let player = Player {
            id: Uuid::new_v4(),
            name: "TestPlayer".to_string(),
            connection_id: Uuid::new_v4(),
            selected_car_config_id: None,
            is_ai: false,
        };

        assert_eq!(player.name, "TestPlayer");
        assert!(!player.is_ai);
    }

    #[test]
    fn test_car_config_default() {
        let car = CarConfig::default();
        assert_eq!(car.name, "Default Car");
        assert!(car.mass_kg > 0.0);
    }

    #[test]
    fn test_track_config_default() {
        let track = TrackConfig::default();
        assert_eq!(track.name, "Default Oval");
        assert!(!track.centerline.is_empty());
        assert!(!track.start_positions.is_empty());
    }

    #[test]
    fn test_race_session_creation() {
        let host_id = Uuid::new_v4();
        let track_id = Uuid::new_v4();
        let session = RaceSession::new(host_id, track_id, SessionKind::Multiplayer, 8, 2, 5);

        assert_eq!(session.host_player_id, host_id);
        assert_eq!(session.track_config_id, track_id);
        assert_eq!(session.max_players, 8);
        assert_eq!(session.ai_count, 2);
        assert_eq!(session.lap_limit, 5);
        assert_eq!(session.state, SessionState::Lobby);
    }

    #[test]
    fn test_player_input_clamp() {
        let mut input = PlayerInputData {
            throttle: 1.5,
            brake: -0.5,
            steering: 2.0,
            gear: None,
            clutch: None,
            drs: false,
            headlights: None,
            flash: false,
        };

        input.clamp();

        assert_eq!(input.throttle, 1.0);
        assert_eq!(input.brake, 0.0);
        assert_eq!(input.steering, 1.0);
    }

    #[test]
    fn test_car_state_creation() {
        let player_id = Uuid::new_v4();
        let car_id = Uuid::new_v4();
        let grid_slot = GridSlot {
            position: 1,
            x: 0.0,
            y: 0.0,
            z: 0.0,
            yaw_rad: 0.0,
        };

        let state = CarState::new(player_id, car_id, &grid_slot);

        assert_eq!(state.player_id, player_id);
        assert_eq!(state.car_config_id, car_id);
        assert_eq!(state.grid_position, 1);
        assert_eq!(state.speed_mps, 0.0);
        assert_eq!(state.current_lap, 0);
    }

    #[test]
    fn dry_weather_leaves_the_track_untouched() {
        let mut track = TrackConfig::default();
        track.centerline.push(TrackPoint {
            grip_modifier: 1.0,
            ..Default::default()
        });
        track.track_surface.base_grip = 1.0;
        track.track_surface.curb_grip = 0.9;
        track.track_surface.off_track_grip = 0.4;
        let before = track.clone();
        for weather in [Weather::Sunny, Weather::Cloudy] {
            let mut copy = before.clone();
            SessionConditions {
                weather,
                ..SessionConditions::DEFAULT
            }
            .apply_to_track(&mut copy);
            assert_eq!(copy.track_surface.base_grip, before.track_surface.base_grip);
            assert_eq!(copy.track_surface.curb_grip, before.track_surface.curb_grip);
            assert_eq!(
                copy.centerline[0].grip_modifier,
                before.centerline[0].grip_modifier
            );
        }
    }

    #[test]
    fn rain_costs_grip_everywhere_and_the_curbs_most() {
        let mut track = TrackConfig::default();
        track.centerline.push(TrackPoint {
            grip_modifier: 1.0,
            ..Default::default()
        });
        track.track_surface.base_grip = 1.0;
        track.track_surface.curb_grip = 0.9;
        track.track_surface.off_track_grip = 0.4;

        let mut last_road = f32::INFINITY;
        for weather in Weather::ALL {
            let mut wet = track.clone();
            SessionConditions {
                weather,
                ..SessionConditions::DEFAULT
            }
            .apply_to_track(&mut wet);
            let road = weather.road_grip_factor();
            assert!(
                road <= last_road,
                "{:?} is no drier than the one before",
                weather
            );
            last_road = road;
            assert!((wet.track_surface.base_grip - road).abs() < 1e-6);
            assert!((wet.centerline[0].grip_modifier - road).abs() < 1e-6);
            // Relative to the road, curbs and grass only ever get worse.
            assert!(wet.track_surface.curb_grip <= 0.9 * road + 1e-6);
            assert!(wet.track_surface.off_track_grip <= 0.4 * road + 1e-6);
        }
        assert!(Weather::HeavyRain.road_grip_factor() < Weather::LightRain.road_grip_factor());
        assert!(Weather::HeavyRain.curb_grip_factor() < 1.0);
        assert!(!Weather::Overcast.is_wet() && Weather::LightRain.is_wet());
    }

    #[test]
    fn conditions_clamp_wraps_the_clock_and_defaults_to_a_sunny_afternoon() {
        assert_eq!(SessionConditions::default(), SessionConditions::DEFAULT);
        assert_eq!(SessionConditions::DEFAULT.time_of_day_minutes, 13 * 60);
        let wrapped = SessionConditions {
            weather: Weather::Overcast,
            time_of_day_minutes: 24 * 60 + 5,
        }
        .clamp();
        assert_eq!(wrapped.time_of_day_minutes, 5);
        assert_eq!(wrapped.weather, Weather::Overcast);
        assert!(wrapped.is_night());
        assert!(!SessionConditions::DEFAULT.is_night());
        assert_eq!(Weather::from_u8(4), Some(Weather::HeavyRain));
        assert_eq!(Weather::from_u8(5), None);
    }

    #[test]
    fn conditions_absent_from_the_wire_decode_to_the_default() {
        // An empty map is what a client from before the field sends inside
        // a `CreateSession` that names `conditions` at all.
        let empty =
            rmp_serde::to_vec_named(&std::collections::BTreeMap::<String, u8>::new()).unwrap();
        let decoded: SessionConditions = rmp_serde::from_slice(&empty).unwrap();
        assert_eq!(decoded, SessionConditions::DEFAULT);

        let full = rmp_serde::to_vec_named(&SessionConditions {
            weather: Weather::LightRain,
            time_of_day_minutes: 1290,
        })
        .unwrap();
        let decoded: SessionConditions = rmp_serde::from_slice(&full).unwrap();
        assert_eq!(decoded.weather, Weather::LightRain);
        assert_eq!(decoded.time_of_day_minutes, 1290);
    }
}
