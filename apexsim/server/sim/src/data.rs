use serde::{Deserialize, Serialize};
use serde_repr::{Deserialize_repr, Serialize_repr};
use uuid::Uuid;

pub use crate::climate::ClimateSite;

// --- Identifiers ---
pub type PlayerId = Uuid;
pub type SessionId = Uuid;
pub type CarConfigId = Uuid;
pub type TrackConfigId = Uuid;
pub type ConnectionId = Uuid;
/// A driver profile's id: made once when the player creates the profile on
/// their machine and sent with `Authenticate`, so a server knows the same
/// driver across connections. A claim, not a proof.
pub type DriverId = Uuid;

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
    /// Share of the drive an AWD car sends to the front axle
    /// (`[drivetrain] awd_front_share`); the rest goes to the rear. Ignored
    /// by FWD and RWD cars.
    #[serde(default = "default_awd_front_share")]
    pub awd_front_share: f32,

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
    /// How the downforce answers ride height and rake (`crate::aero`);
    /// the default is a car whose downforce does not.
    #[serde(default)]
    pub aero: crate::aero::AeroConfig,
    /// What the brakes are made of (`crate::brakes`): carbon for the
    /// prototypes and F1, steel otherwise, unless car.toml's `[brakes]`
    /// says.
    #[serde(default)]
    pub brake_material: crate::brakes::BrakeMaterial,
    /// The brake ducts' size against the car's own, per axle (the setup's
    /// `brake_ducts` and `brake_ducts_rear`): how much air cools the brakes.
    #[serde(default = "default_one_f32")]
    pub brake_duct_scale: f32,
    #[serde(default = "default_one_f32")]
    pub brake_duct_scale_rear: f32,
    /// The pads fitted (the setup's `brake_pads`; `crate::brakes`): what
    /// they grip with, how fast they wear and where they come in.
    #[serde(default)]
    pub brake_pads: crate::brakes::BrakePads,
    /// How fast the discs wear, percent per megajoule absorbed in their
    /// window (`[brakes] disc_wear_per_mj`; `crate::brakes::disc_wear_percent`).
    /// 0, the default, is discs that never wear.
    #[serde(default)]
    pub brake_disc_wear_per_mj: f32,

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
    /// Turbo lag (`[engine.turbo]`), for a car whose torque curve already
    /// carries its boost. `None`: the curve is delivered the instant the
    /// throttle opens, as it always was.
    #[serde(default)]
    pub turbo: Option<TurboConfig>,
    /// A turbo- or supercharged engine: its boost control holds most of its
    /// power where the air is thin (`physics::engine_density_factor`).
    /// Apart from the lag model, which the shipped cars do not use.
    #[serde(default)]
    pub forced_induction: bool,
    /// The radiator against the one sized for this engine
    /// (`engine_heat::radiator_conductance`): under 1 runs hotter.
    #[serde(default = "default_one_f32")]
    pub radiator_scale: f32,
}

/// How long a turbo takes to deliver the boosted part of the torque curve.
///
/// The curve is the engine at full boost (an Assetto Corsa import bakes
/// `power.lut x (1 + boost)` into it). Of that, `1 - boosted_share` is the
/// engine without boost and arrives with the throttle; the rest follows a
/// first-order spool toward the throttle opening, `lag_up_s` to build and
/// `lag_down_s` to bleed off. Only a tip-in waits for it: at a steady
/// pedal the spool has caught up and the curve is delivered exactly, and a
/// lift never costs torque the pedal still asks for.
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
pub struct TurboConfig {
    /// Share of the curve's torque that is boost, 0..0.9.
    pub boosted_share: f32,
    /// Spool-up time constant, s.
    pub lag_up_s: f32,
    /// Spool-down time constant, s.
    pub lag_down_s: f32,
}

impl TurboConfig {
    /// The spool one tick on from `spool`, chasing the throttle opening.
    pub fn spool_toward(&self, spool: f32, throttle: f32, dt: f32) -> f32 {
        let target = throttle.clamp(0.0, 1.0);
        let tau = if target > spool {
            self.lag_up_s
        } else {
            self.lag_down_s
        };
        let step = if tau > 0.0 {
            1.0 - (-dt / tau).exp()
        } else {
            1.0
        };
        (spool + (target - spool) * step).clamp(0.0, 1.0)
    }

    /// Share of the curve delivered at `throttle` with the turbo at
    /// `spool`: 1 once the spool has reached the pedal.
    pub fn torque_factor(&self, spool: f32, throttle: f32) -> f32 {
        let ready = if throttle > 0.0 {
            (spool / throttle).min(1.0)
        } else {
            1.0
        };
        1.0 - self.boosted_share * (1.0 - ready)
    }
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
            turbo: None,
            forced_induction: false,
            radiator_scale: 1.0,
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
    /// The gearbox refuses a downshift that would spin the engine past its
    /// limit, as a modern racing car's does: the shift drops to the lowest
    /// gear that fits, or not at all (`physics::protected_gear`). Off, a
    /// driver can over-rev the engine with a downshift.
    #[serde(default)]
    pub downshift_protection: bool,
}

impl Default for TransmissionConfig {
    fn default() -> Self {
        Self {
            transmission_type: TransmissionType::Sequential,
            shift_time_s: 0.12,
            efficiency: 0.92,
            downshift_protection: false,
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
    /// Whether the physics runs the differential at all
    /// (`[differential] simulated = true`). Every car's table predates the
    /// model and its lap time was set without one, so a car only gets a
    /// differential by asking for it; without, each driven wheel gets half
    /// its axle's torque whatever the other can carry.
    #[serde(default)]
    pub simulated: bool,
}

impl Default for DifferentialConfig {
    fn default() -> Self {
        Self {
            differential_type: DifferentialType::ClutchLSD,
            preload_nm: 60.0,
            lock_power: 0.35,
            lock_coast: 0.20,
            simulated: false,
        }
    }
}

/// Lower heating value of racing petrol, J/kg: the chemical energy a
/// kilogram of fuel releases, of which the engine turns
/// [`FuelConfig::thermal_efficiency`] into work at the crank.
pub const FUEL_ENERGY_J_PER_KG: f32 = 43.0e6;

/// The fuel system: the tank, what the fuel weighs and where, and how fast
/// the engine burns it.
///
/// `CarConfig::mass_kg` is the car with its driver and a dry tank (as AC's
/// `TOTALMASS` is), so what is in the tank is carried on top of it.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct FuelConfig {
    pub capacity_liters: f32,
    /// Base consumption at idle (L/s)
    pub idle_consumption_lps: f32,
    /// The old consumption rule, `throttle x rpm / max_rpm x scale` L/s on
    /// top of idle, kept for a car.toml that still names it and no
    /// `thermal_efficiency`. `None` burns fuel by the power the engine
    /// makes instead.
    #[serde(default)]
    pub load_consumption_scale: Option<f32>,
    /// Share of the fuel's energy the engine turns into work at the crank
    /// (brake thermal efficiency): about 0.30-0.35 for a GT or LMP engine,
    /// 0.40 for a hypercar's, 0.50 for a current F1 power unit.
    #[serde(default = "default_thermal_efficiency")]
    pub thermal_efficiency: f32,
    /// kg per litre.
    #[serde(default = "default_fuel_density")]
    pub density_kg_per_l: f32,
    /// Share of the fuel's weight on the front axle: where the tank sits.
    /// `None` puts it at the car's own weight split, so a draining tank
    /// lightens the car without moving its balance.
    #[serde(default)]
    pub tank_front_share: Option<f32>,
    /// The whole car's energy per stint, MJ (the WEC's rule): the fuel's
    /// chemical energy burnt plus what the hybrid's motor deploys, from a
    /// pit stop, a grid or the garage to the next stop. Spent, the engine
    /// cuts as on a dry tank and the motor stops. `None`, the default, is
    /// no such limit.
    #[serde(default)]
    pub stint_energy_mj: Option<f32>,
}

fn default_thermal_efficiency() -> f32 {
    0.30
}

fn default_fuel_density() -> f32 {
    0.745
}

impl Default for FuelConfig {
    fn default() -> Self {
        Self {
            capacity_liters: 100.0,
            idle_consumption_lps: 0.00005,
            load_consumption_scale: None,
            thermal_efficiency: default_thermal_efficiency(),
            density_kg_per_l: default_fuel_density(),
            tank_front_share: None,
            stint_energy_mj: None,
        }
    }
}

impl FuelConfig {
    /// What `liters` of fuel weigh, kg.
    pub fn mass_kg(&self, liters: f32) -> f32 {
        liters.max(0.0) * self.density_kg_per_l
    }

    /// The fuel's chemical energy in `liters`, MJ.
    pub fn energy_mj(&self, liters: f32) -> f32 {
        self.mass_kg(liters) * FUEL_ENERGY_J_PER_KG * 1e-6
    }

    /// Litres holding `mj` of the fuel's chemical energy.
    pub fn liters_for_mj(&self, mj: f32) -> f32 {
        mj.max(0.0) * 1e6 / (FUEL_ENERGY_J_PER_KG * self.density_kg_per_l.max(0.1))
    }

    /// The stint's energy left, MJ, with `used_mj` spent: infinite without
    /// a `stint_energy_mj`.
    pub fn stint_left_mj(&self, used_mj: f32) -> f32 {
        self.stint_energy_mj
            .map_or(f32::INFINITY, |limit| limit - used_mj)
    }

    /// The stint's energy left as telemetry sends it: percent of
    /// `stint_energy_mj`, 255 without one.
    pub fn stint_pct(&self, used_mj: f32) -> u8 {
        match self.stint_energy_mj {
            Some(limit) if limit > 0.0 => {
                ((1.0 - used_mj / limit) * 100.0).round().clamp(0.0, 100.0) as u8
            }
            _ => 255,
        }
    }

    /// Litres per second the engine burns making `crank_power_w` at the
    /// crank (combustion only: engine braking and a fuel cut burn none).
    pub fn burn_lps(&self, crank_power_w: f32) -> f32 {
        let per_joule = 1.0
            / (self.thermal_efficiency.max(0.05)
                * FUEL_ENERGY_J_PER_KG
                * self.density_kg_per_l.max(0.1));
        crank_power_w.max(0.0) * per_joule
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
    /// Energy the motor may deploy between two crossings of the line, kJ
    /// (`crate::hybrid`); `None` is no limit but the battery.
    #[serde(default)]
    pub deploy_kj_per_lap: Option<f32>,
    /// The motor drives only above this speed, km/h (a hypercar's rule).
    #[serde(default)]
    pub deploy_min_speed_kph: f32,
    /// A turbo generator charging the battery at full throttle, kW.
    #[serde(default)]
    pub heat_recovery_kw: f32,
    /// Energy the motor may deploy in one stint, kJ (the WEC's rule): from
    /// a pit stop, a grid or the garage to the next stop. `None` is no
    /// stint limit.
    #[serde(default)]
    pub stint_kj: Option<f32>,
    /// Energy the overtake button may spend per lap, kJ, over and above the
    /// paced lap budget (`deploy_kj_per_lap`); `None` lets the button draw
    /// on the lap budget as Attack does.
    #[serde(default)]
    pub override_kj_per_lap: Option<f32>,
    /// Brake-by-wire: under braking the motor's recovery stands in for the
    /// driven axle's hydraulic brakes, so the pedal gives the same
    /// deceleration whether the battery takes the energy or the discs do.
    /// Without it the recovery brakes the driven axle on top of the pads.
    #[serde(default = "default_true")]
    pub brake_by_wire: bool,
    /// How long brake-by-wire takes to follow the recovery, s: the time
    /// constant of the driven axle's hydraulics answering a change in what
    /// the motor recovers (`hybrid::bbw_offset_n`). While it catches up the
    /// balance migrates: rearward as the recovery comes in at the start of
    /// a stop, forward as it drops away when the battery fills. 0 (the
    /// default) follows it at once.
    #[serde(default)]
    pub bbw_response_s: f32,
    /// Balanced mode's spend curve (`crate::hybrid`), from the setup's
    /// deploy map, never the car.toml: 0 spends the lap budget evenly over
    /// the lap, positive earlier, negative later.
    #[serde(default)]
    pub deploy_early: f32,
    /// What the motor drives (`[hybrid] axle`): the crank, behind the
    /// clutch and through the gearbox to the driven wheels (the default),
    /// or the front axle directly, a hypercar's motor, whatever the
    /// engine drives. With the front axle `motor_max_torque_nm` is the
    /// torque at the axle and the motor turns with the front wheels.
    #[serde(default)]
    pub axle: HybridAxle,
    /// The motor's torque by its rpm (`[[hybrid.torque_curve]]`: the
    /// crank's rpm, or the front wheels' on the front axle), capped at
    /// `motor_max_torque_nm`; empty (the default) is the peak at every
    /// speed, up to the power limit.
    #[serde(default)]
    pub torque_curve: Vec<TorqueCurvePoint>,
}

impl HybridConfig {
    /// The torque the motor can give at `rpm`: its curve where it files
    /// one, else its peak.
    pub fn torque_at(&self, rpm: f32) -> f32 {
        if self.torque_curve.is_empty() {
            return self.motor_max_torque_nm;
        }
        crate::physics::interpolate_torque_curve(&self.torque_curve, rpm)
            .clamp(0.0, self.motor_max_torque_nm)
    }
}

/// Where a hybrid's motor puts its torque (`HybridConfig::axle`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Serialize, Deserialize)]
pub enum HybridAxle {
    /// At the crank, through the gearbox to the driven wheels.
    #[default]
    Crank,
    /// Straight onto the front axle (an LMH's motor): an RWD car is driven
    /// at both ends while it deploys and recovers on the front.
    Front,
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
            deploy_kj_per_lap: None,
            deploy_min_speed_kph: 0.0,
            heat_recovery_kw: 0.0,
            stint_kj: None,
            override_kj_per_lap: None,
            brake_by_wire: true,
            bbw_response_s: 0.0,
            deploy_early: 0.0,
            axle: HybridAxle::Crank,
            torque_curve: Vec::new(),
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
    /// Suspension geometry (`crate::geometry`). Static camber per axle,
    /// degrees (negative: the top of the wheel toward the car), and what
    /// the car.toml filed, which the grip is measured against (a setup
    /// moves the first, never the second); how much of the body's roll
    /// the linkage gains back (0..1); and whether the car.toml gave any
    /// camber at all (without, camber changes nothing).
    #[serde(default)]
    pub camber_front_deg: f32,
    #[serde(default)]
    pub camber_rear_deg: f32,
    #[serde(default)]
    pub camber_filed_front_deg: f32,
    #[serde(default)]
    pub camber_filed_rear_deg: f32,
    #[serde(default)]
    pub camber_gain_front: f32,
    #[serde(default)]
    pub camber_gain_rear: f32,
    #[serde(default)]
    pub camber_modelled: bool,
    /// Toe per wheel, degrees, positive toe-in.
    #[serde(default)]
    pub toe_front_deg: f32,
    #[serde(default)]
    pub toe_rear_deg: f32,
    /// The bump stop engages this far past the static laden compression,
    /// m, at this rate, N/m; none without a gap. The per-axle gaps win
    /// over the shared one where a car.toml gives them.
    #[serde(default)]
    pub bump_stop_gap_m: Option<f32>,
    #[serde(default)]
    pub bump_stop_gap_front_m: Option<f32>,
    #[serde(default)]
    pub bump_stop_gap_rear_m: Option<f32>,
    #[serde(default)]
    pub bump_stop_rate_n_per_m: f32,
    /// Roll centre height per axle, m above the road (`crate::geometry`):
    /// the share of an axle's lateral load transfer that goes through its
    /// linkage at once (geometric) rather than through its springs and bar
    /// (elastic). `None` on both is the old transfer, all of it elastic.
    #[serde(default)]
    pub roll_centre_front_m: Option<f32>,
    #[serde(default)]
    pub roll_centre_rear_m: Option<f32>,
    /// Camber thrust: the side force a leaning tyre makes at no slip, as a
    /// share of its cornering stiffness per radian of lean
    /// (`geometry::camber_thrust_rad`). 0 is none.
    #[serde(default)]
    pub camber_thrust: f32,
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
            camber_front_deg: 0.0,
            camber_rear_deg: 0.0,
            camber_filed_front_deg: 0.0,
            camber_filed_rear_deg: 0.0,
            camber_gain_front: 0.0,
            camber_gain_rear: 0.0,
            camber_modelled: false,
            toe_front_deg: 0.0,
            toe_rear_deg: 0.0,
            bump_stop_gap_m: None,
            bump_stop_gap_front_m: None,
            bump_stop_gap_rear_m: None,
            bump_stop_rate_n_per_m: 0.0,
            roll_centre_front_m: None,
            roll_centre_rear_m: None,
            camber_thrust: 0.0,
        }
    }
}

impl SuspensionConfig {
    /// The bump stop gap on an axle, m: its own, else the shared one.
    pub fn bump_stop_gap(&self, front: bool) -> Option<f32> {
        if front {
            self.bump_stop_gap_front_m.or(self.bump_stop_gap_m)
        } else {
            self.bump_stop_gap_rear_m.or(self.bump_stop_gap_m)
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TireConfig {
    pub grip_coefficient: f32,       // Base grip coefficient (0.8-1.2)
    pub optimal_slip_ratio: f32,     // Peak longitudinal slip (typically 0.06-0.12)
    pub optimal_slip_angle_rad: f32, // Peak lateral slip angle (typically 6-10 degrees)
    pub rolling_resistance: f32,     // Rolling resistance coefficient
    /// The middle of the compound's working window, °C: the temperature
    /// the tyre grips best at (`crate::tyre_thermal`). The running
    /// pressures below are the pressures *at* this temperature.
    pub optimal_temperature_c: f32,
    /// Grip lost per degree outside the window (a share of the peak, so
    /// 0.004 is 0.4% a degree), eased in over the first few degrees.
    pub temperature_grip_falloff: f32,
    /// Half-width of the window, °C: within `optimal ± window` the tyre
    /// grips its full figure.
    #[serde(default = "default_temperature_window_c")]
    pub temperature_window_c: f32,
    /// Tyre blankets: the temperature a car is sent out on, °C. `None`
    /// (no blankets) sends it out at the air temperature.
    #[serde(default)]
    pub blanket_temperature_c: Option<f32>,
    pub wear_rate: f32, // Wear rate multiplier
    /// Running pressure per axle and the pressure the tyre grips best at.
    /// Off the optimum the contact patch shrinks (or crowns) and the axle
    /// loses grip quadratically (`pressure_grip_factor`); the garage setup
    /// moves the running pressures, which is how it shifts the balance.
    /// A running pressure is the *hot* one, at `optimal_temperature_c`:
    /// the gas in a colder tyre is at less (`tyre_thermal::pressure_kpa`),
    /// in a hotter one at more.
    #[serde(default = "default_tyre_pressure_kpa")]
    pub pressure_front_kpa: f32,
    #[serde(default = "default_tyre_pressure_kpa")]
    pub pressure_rear_kpa: f32,
    #[serde(default = "default_tyre_pressure_kpa")]
    pub optimal_pressure_kpa: f32,
    /// Load sensitivity per axle, the exponent of the peak force in the
    /// load (Assetto Corsa's `LS_EXPY`): the peak friction coefficient is
    /// `mu x (Fz / Fz_ref)^(load_sensitivity - 1)`, so below 1 a tyre
    /// pressed harder grips less per newton, which is what makes load
    /// transfer cost grip. 1.0 is the linear tyre every car had before.
    #[serde(default = "default_one")]
    pub load_sensitivity_front: f32,
    #[serde(default = "default_one")]
    pub load_sensitivity_rear: f32,
    /// The load at which a tyre grips exactly its axle's coefficient
    /// (`Fz_ref`), N. `None`: the axle's own static per-wheel load, so the
    /// car standing still grips as `grip_coefficient` says and the racing
    /// line and the AI, which read that figure, stay honest.
    #[serde(default)]
    pub reference_load_front_n: Option<f32>,
    #[serde(default)]
    pub reference_load_rear_n: Option<f32>,
    /// Longitudinal peak over the lateral one (AC's `DX0 / DY0`): the
    /// friction circle becomes an ellipse this much longer along the
    /// tyre. 1.0 is the circle.
    #[serde(default = "default_one")]
    pub longitudinal_grip_factor: f32,
    /// Per-axle multipliers on `grip_coefficient`, for a car with a
    /// different compound front and rear. 1.0 on both is one compound.
    #[serde(default = "default_one")]
    pub front_grip_scale: f32,
    #[serde(default = "default_one")]
    pub rear_grip_scale: f32,
    /// The car's own compounds (car.toml `[[tires.compound]]`,
    /// `crate::tyre_thermal`); empty means the default five.
    #[serde(default)]
    pub compounds: Vec<crate::tyre_thermal::Compound>,
}

fn default_tyre_pressure_kpa() -> f32 {
    180.0
}

fn default_temperature_window_c() -> f32 {
    crate::tyre_thermal::DEFAULT_WINDOW_C
}

fn default_one() -> f32 {
    1.0
}

/// Load over the reference is clamped to this range before the load
/// sensitivity sees it, so an unloaded wheel (a kerb, a crest) does not
/// get an unbounded coefficient from a negative exponent.
const LOAD_SENSITIVITY_RATIO_RANGE: (f32, f32) = (0.1, 5.0);
/// Passes `CarConfig::envelope_mu` makes at the lateral transfer of a
/// load-sensitive tyre's cornering limit.
const ENVELOPE_TRANSFER_PASSES: usize = 4;

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

    /// Nothing but `grip_coefficient` decides the peak: no load
    /// sensitivity, one compound. The racing line and the AI take the
    /// coefficient as it is for such a tyre, to the bit.
    pub fn is_linear_single_compound(&self) -> bool {
        self.load_sensitivity_front == 1.0
            && self.load_sensitivity_rear == 1.0
            && self.front_grip_scale == 1.0
            && self.rear_grip_scale == 1.0
    }
}

impl CarConfig {
    /// The car as it stands with `fuel_liters` in the tank: its mass, kg,
    /// and the share of it on the front axle.
    pub fn laden(&self, fuel_liters: f32) -> (f32, f32) {
        let fuel = self.fuel.mass_kg(fuel_liters);
        let mass = self.mass_kg + fuel;
        if fuel <= 0.0 {
            return (self.mass_kg, self.weight_distribution_front);
        }
        let tank = self
            .fuel
            .tank_front_share
            .unwrap_or(self.weight_distribution_front);
        let front = (self.mass_kg * self.weight_distribution_front + fuel * tank) / mass;
        (mass, front)
    }

    /// Static load on one wheel of the axle, N.
    pub fn static_wheel_load_n(&self, front: bool) -> f32 {
        let share = if front {
            self.weight_distribution_front
        } else {
            1.0 - self.weight_distribution_front
        };
        self.mass_kg * 9.81 * share / 2.0
    }

    /// Multiplier the load sensitivity puts on an axle's coefficient for a
    /// tyre carrying `load_n`: exactly 1.0 for a linear tyre, whatever the
    /// load, so a car without the key is the car it was.
    pub fn load_sensitivity_factor(&self, front: bool, load_n: f32) -> f32 {
        let tyre = &self.tire_config;
        let (exponent, reference) = if front {
            (tyre.load_sensitivity_front, tyre.reference_load_front_n)
        } else {
            (tyre.load_sensitivity_rear, tyre.reference_load_rear_n)
        };
        if exponent == 1.0 {
            return 1.0;
        }
        let reference = reference
            .unwrap_or_else(|| self.static_wheel_load_n(front))
            .max(1.0);
        let (lo, hi) = LOAD_SENSITIVITY_RATIO_RANGE;
        (load_n / reference).clamp(lo, hi).powf(exponent - 1.0)
    }

    /// Peak friction coefficient of one of the axle's tyres at `load_n`,
    /// before the surface and the tyre pressure: `grip_coefficient` times
    /// the axle's compound and its load sensitivity.
    pub fn axle_tyre_mu(&self, front: bool, load_n: f32) -> f32 {
        let scale = if front {
            self.tire_config.front_grip_scale
        } else {
            self.tire_config.rear_grip_scale
        };
        self.tire_config.grip_coefficient * scale * self.load_sensitivity_factor(front, load_n)
    }

    /// The car's friction coefficient as a point mass whose weight is
    /// `load_ratio` times its static weight (1 + downforce / weight): what
    /// the racing line and the AI's grip envelope plan with. For a linear,
    /// single-compound tyre it is `grip_coefficient` itself; otherwise the
    /// two axles' coefficients at that share of their static load,
    /// weighted by the static weight split.
    ///
    /// A load-sensitive tyre is taken at the limit of a corner, as the
    /// physics loads it: each axle's outside wheel carries the lateral
    /// transfer (`geometry::lateral_transfer`) and grips less per newton
    /// than its inside wheel gains, so the axle is the two wheels' grip over
    /// its load; and the weaker axle holds the car, each carrying its share
    /// of the cornering by the weight on it. The transfer is that of
    /// cornering at the coefficient found,
    /// solved again a few times (it settles at once). Read at the static
    /// load alone, an imported car's light F1 planned 8% over its grip and
    /// its AI slid through every fast corner.
    pub fn envelope_mu(&self, load_ratio: f32) -> f32 {
        if self.tire_config.is_linear_single_compound() {
            return self.tire_config.grip_coefficient;
        }
        let w_f = self.weight_distribution_front;
        let at =
            |front: bool| self.axle_tyre_mu(front, self.static_wheel_load_n(front) * load_ratio);
        let flat = w_f * at(true) + (1.0 - w_f) * at(false);
        let tyre = &self.tire_config;
        if tyre.load_sensitivity_front == 1.0 && tyre.load_sensitivity_rear == 1.0 {
            return flat;
        }
        let axle = |front: bool, transfer: f32| {
            let load = (self.static_wheel_load_n(front) * load_ratio).max(1.0);
            let outside = load + transfer.abs();
            let inside = (load - transfer.abs()).max(0.0);
            (self.axle_tyre_mu(front, outside) * outside
                + self.axle_tyre_mu(front, inside) * inside)
                / (2.0 * load)
        };
        let mut mu = flat;
        for _ in 0..ENVELOPE_TRANSFER_PASSES {
            let accel = mu * load_ratio * 9.81;
            let (front, rear) = crate::geometry::lateral_transfer(self, self.mass_kg, w_f, accel);
            mu = axle(true, front).min(axle(false, rear));
        }
        mu
    }

    /// Front and rear shares of the drive for an AWD car.
    pub fn awd_shares(&self) -> (f32, f32) {
        let front = self.awd_front_share;
        // The literal pair the split was hardcoded as: `1.0 - 0.4` is not
        // `0.6` in f32, and the shipped AWD cars must drive to the bit.
        if front == DEFAULT_AWD_FRONT_SHARE {
            (0.4, 0.6)
        } else {
            (front, 1.0 - front)
        }
    }
}

impl Default for TireConfig {
    fn default() -> Self {
        Self {
            grip_coefficient: 1.0,
            optimal_slip_ratio: 0.08,
            optimal_slip_angle_rad: 0.12, // ~7 degrees
            rolling_resistance: 0.015,
            optimal_temperature_c: crate::tyre_thermal::DEFAULT_OPTIMAL_C,
            temperature_grip_falloff: crate::tyre_thermal::DEFAULT_GRIP_FALLOFF,
            temperature_window_c: crate::tyre_thermal::DEFAULT_WINDOW_C,
            blanket_temperature_c: None,
            wear_rate: 1.0,
            pressure_front_kpa: 180.0,
            pressure_rear_kpa: 180.0,
            optimal_pressure_kpa: 180.0,
            load_sensitivity_front: 1.0,
            load_sensitivity_rear: 1.0,
            reference_load_front_n: None,
            reference_load_rear_n: None,
            longitudinal_grip_factor: 1.0,
            front_grip_scale: 1.0,
            rear_grip_scale: 1.0,
            compounds: Vec::new(),
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
            awd_front_share: DEFAULT_AWD_FRONT_SHARE,

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
            aero: crate::aero::AeroConfig::default(),
            brake_material: crate::brakes::BrakeMaterial::Steel,
            brake_duct_scale: 1.0,
            brake_duct_scale_rear: 1.0,
            brake_pads: crate::brakes::BrakePads::Standard,
            brake_disc_wear_per_mj: 0.0,

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
    /// Path to the source track file, relative to the content folder (e.g. "tracks/default/Armadillos/Armadillos.yaml")
    #[serde(default)]
    pub source_path: Option<String>,
    /// Checksum of the track file this was loaded from (`content_crc`), 0
    /// for a track built in memory. Sent to clients so they can tell whether
    /// their baked level matches.
    #[serde(default)]
    pub content_crc: u32,
    pub start_positions: Vec<GridSlot>,
    pub track_surface: TrackSurface,
    /// The pit lane and its boxes, from `<Track>.pit.msgpack` beside the
    /// track file (`crate::pit`); `None` for a circuit without one (no pit
    /// stops). Runtime-only; loaded from the sidecar.
    #[serde(skip)]
    pub pit_lane: Option<crate::pit::PitLane>,
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
    /// The road through the session (`crate::road_state`): the rubber the
    /// cars lay, the marbles they shed, the water standing in the dips and
    /// the lines they dry. Built by the session (`GameSession::new`);
    /// `None` on a track nobody races on, which grips as baked.
    /// Runtime-only.
    #[serde(skip)]
    pub road_state: Option<crate::road_state::RoadState>,
    /// Baked road mesh exported alongside the Unreal scene
    /// (`<Track>.road.msgpack`): the rendered road, curbs, run-off bands
    /// and pit lane as triangles, each with its surface. When present the
    /// wheels take their height, normal and surface class from it instead
    /// of the centerline formula (`physics::query_track_surface`). Loaded
    /// only when the server's `[physics] road_contact` is `"mesh"`, so a
    /// track without it, or a server that has not opted in, drives on the
    /// centerline as before. Shared between a session's copies of the
    /// track: it is read-only and can be tens of megabytes.
    #[serde(skip)]
    pub road_mesh: Option<std::sync::Arc<crate::road_mesh::RoadMesh>>,
    /// The road has the crown and bumps of
    /// [`apexsim_formats::road_profile`], which the content's road mesh is
    /// baked with too: set by the loader for every circuit read from a
    /// track file but an imported one (`TrackFile::has_road_profile`); a
    /// track built in code has a flat road.
    #[serde(default)]
    pub road_profile: bool,
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

/// The track file's DRS zones, racing line points and metadata are the
/// file's own types, from the formats crate
/// (`apexsim-lib/formats/src/track.rs`), carried on the runtime track as
/// they were read.
pub use apexsim_formats::track::{DrsZone, RacelinePoint, TrackMetadata};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TrackSurface {
    pub base_grip: f32,      // Base grip multiplier (1.0 = normal asphalt)
    pub curb_grip: f32,      // Grip on curbs
    pub off_track_grip: f32, // Grip off track (grass/gravel)
    /// Rolling resistance off track: each tyre on the grass is held back
    /// by this share of the load it carries, along its rolling direction
    /// at its own contact patch, so two wheels dropped onto the grass drag
    /// that side and pull the nose toward it. A share of the load rather
    /// than a fraction of speed per second: grass is already slow through
    /// its grip, and a proportional drag fought the throttle so hard that
    /// a car could not get back above a crawl.
    pub off_track_rolling_resistance: f32,
    /// The air, °C: what cools the tyres, and what they go out at without
    /// blankets. Baked from the session's weather and clock
    /// (`SessionConditions::apply_to_track`).
    #[serde(default = "default_air_temperature_c")]
    pub air_temperature_c: f32,
    /// The asphalt, °C: what the tread touches.
    #[serde(default = "default_track_temperature_c")]
    pub track_temperature_c: f32,
    /// Standing water: the road draws heat out of the tyres several times
    /// faster (`tyre_thermal`).
    #[serde(default)]
    pub wet: bool,
    /// How much water is on the road, 0 dry to 1 heavy rain
    /// (`Weather::water`): which tyre grips (`tyre_thermal::Compound`).
    /// The road's baked grip is the grip on the tyre the weather calls for.
    #[serde(default)]
    pub water: f32,
    /// The water the road's grip was baked at
    /// (`SessionConditions::apply_to_track`): `water` moves with the sky
    /// through a session (`crate::conditions`), and every grip the road
    /// state works out is against this one (`crate::road_state`).
    #[serde(default)]
    pub baked_water: f32,
    /// Whether the road's grip was baked snow-covered (1 under a snow
    /// sky, else 0): the snow grip the road state works out is against it.
    #[serde(default)]
    pub baked_snow: f32,
    /// The air's density against the reference day the cars are filed at
    /// (`SessionConditions::air_density_ratio`): the aero and a combustion
    /// engine's torque are scaled by it. 1.0 on a default day at sea level.
    #[serde(default = "default_one_f32")]
    pub air_density_ratio: f32,
    /// The session's mean wind, m/s toward where it blows, track frame.
    #[serde(default)]
    pub wind_mps: [f32; 2],
    /// The wind this tick, gusts included (`GameSession` sets it before the
    /// physics from the mean, `wind::gusting`).
    #[serde(default)]
    pub wind_now_mps: [f32; 2],
}

fn default_one_f32() -> f32 {
    1.0
}

fn default_medium() -> u8 {
    crate::tyre_thermal::MEDIUM
}

fn default_air_temperature_c() -> f32 {
    SessionConditions::DEFAULT.air_temperature_c()
}

fn default_track_temperature_c() -> f32 {
    SessionConditions::DEFAULT.track_temperature_c()
}

/// Grip on the grass against the road's: a slick on dry turf keeps about
/// a third of what it has on asphalt (it was 0.6 on the real circuits,
/// and two wheels on the grass felt like the road). The patch under each
/// tyre varies about it (`physics::grass_grip_patch`).
pub const OFF_TRACK_GRIP: f32 = 0.35;

/// Rolling resistance on grass, a share of each tyre's load: about 0.06,
/// a car tyre's on turf. Kept well under what grass grip can put down, or
/// the car cannot pull away.
pub const OFF_TRACK_ROLLING_RESISTANCE: f32 = 0.06;

impl Default for TrackSurface {
    fn default() -> Self {
        Self {
            base_grip: 1.0,
            curb_grip: 0.85,
            off_track_grip: OFF_TRACK_GRIP,
            off_track_rolling_resistance: OFF_TRACK_ROLLING_RESISTANCE,
            air_temperature_c: default_air_temperature_c(),
            track_temperature_c: default_track_temperature_c(),
            wet: false,
            water: 0.0,
            baked_water: 0.0,
            baked_snow: 0.0,
            air_density_ratio: 1.0,
            wind_mps: [0.0; 2],
            wind_now_mps: [0.0; 2],
        }
    }
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
            ground: None,
            curbs: None,
            walls: None,
            road_state: None,
            road_mesh: None,
            road_profile: false,
        }
    }
}

/// A baked sidecar of a circuit (`ats-export`, gitignored): what a test
/// that drives one names before it starts ([`TrackConfig::has_sidecars`]).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Sidecar {
    /// `<Stem>.ground.msgpack`: the verge and the terrain off the road.
    Ground,
    /// `<Stem>.curbs.msgpack`: the curbs and run-off the track limits count.
    Curbs,
    /// `<Stem>.walls.msgpack`: the barriers.
    Walls,
    /// `<Stem>.pit.msgpack`: the pit lane.
    Pit,
    /// `<Stem>.road.msgpack`: the road mesh (loaded only on that backend).
    Road,
}

impl Sidecar {
    /// The road a car drives on a baked tree: a test whose result depends
    /// on the curbs, walls or verge (a lap's limits, a line through the
    /// kerbs, a car running wide) names these.
    pub const BAKED_ROAD: &'static [Sidecar] = &[Sidecar::Ground, Sidecar::Curbs, Sidecar::Walls];

    pub fn name(self) -> &'static str {
        match self {
            Sidecar::Ground => "ground",
            Sidecar::Curbs => "curbs",
            Sidecar::Walls => "walls",
            Sidecar::Pit => "pit",
            Sidecar::Road => "road",
        }
    }
}

impl TrackConfig {
    /// Whether the track was loaded with every sidecar of `need`. When it
    /// was not (a checkout that has not baked its tracks, as CI), says so
    /// on stderr with `what` (the test or the part of it that needs them),
    /// so the test can skip that part with its reason rather than race a
    /// different road and fail.
    pub fn has_sidecars(&self, need: &[Sidecar], what: &str) -> bool {
        let missing: Vec<&str> = need
            .iter()
            .filter(|s| match s {
                Sidecar::Ground => self.ground.is_none(),
                Sidecar::Curbs => self.curbs.is_none(),
                Sidecar::Walls => self.walls.is_none(),
                Sidecar::Pit => self.pit_lane.is_none(),
                Sidecar::Road => self.road_mesh.is_none(),
            })
            .map(|s| s.name())
            .collect();
        if !missing.is_empty() {
            eprintln!(
                "{what}: skipped, {} has no {} sidecar (run ats-export)",
                self.name,
                missing.join(", ")
            );
        }
        missing.is_empty()
    }

    /// Where the circuit is, (degrees north, degrees east), when its
    /// metadata names a longitude: what `SessionConditions::at_site` works
    /// solar noon out from. None for a track that does not say.
    pub fn site(&self) -> Option<(f32, f32)> {
        let longitude = self.metadata.longitude_deg.filter(|l| l.is_finite())?;
        Some((self.latitude_deg(), longitude))
    }

    /// Where the circuit is as its weather sees it: its latitude, its
    /// longitude when the metadata names one, its altitude (sea level when
    /// it does not say).
    pub fn climate_site(&self) -> ClimateSite {
        ClimateSite {
            latitude_deg: self.latitude_deg(),
            longitude_deg: self.metadata.longitude_deg.filter(|l| l.is_finite()),
            altitude_m: self
                .metadata
                .altitude_m
                .filter(|a| a.is_finite())
                .unwrap_or(0.0),
        }
    }

    /// The circuit's latitude, degrees north, or the sky model's default
    /// for a track that does not say where it is.
    pub fn latitude_deg(&self) -> f32 {
        self.metadata
            .latitude_deg
            .filter(|l| l.is_finite())
            .unwrap_or(DEFAULT_LATITUDE_DEG)
    }

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
    /// The tread, °C: the rubber on the road, quick to heat in a slide and
    /// quick to cool on a straight (`crate::tyre_thermal`); the mean of
    /// its three zones.
    pub temperature_c: f32,
    /// The tread across its width, °C: inner shoulder (toward the car),
    /// middle, outer shoulder.
    #[serde(default)]
    pub tread_c: [f32; 3],
    /// Running (gauge) pressure, kPa: the setup's hot pressure moved by the
    /// temperature of the gas, which is the core's.
    pub pressure_kpa: f32,
    pub wear_percent: f32,
    pub slip_ratio: f32,
    pub slip_angle_rad: f32,
    /// The carcass and the gas inside it, °C: slow, heated by the tyre
    /// flexing and by the tread.
    #[serde(default)]
    pub core_temperature_c: f32,
    /// What the tyre's temperature and pressure leave of its grip, as a
    /// share of what it has in its window at the set pressure (1.0).
    #[serde(default)]
    pub grip_factor: f32,
    /// How deep a flat spot a locked wheel has ground into the tread, 0..1
    /// (`crate::tyre_thermal`): a little grip, and a shake once a turn.
    #[serde(default)]
    pub flat_spot: f32,
    /// How long the wheel has been locked without a break, s: a flat spot
    /// is only ground once a lock-up holds (`crate::tyre_thermal`).
    #[serde(default)]
    pub lock_s: f32,
    /// Worn through, or holed: the tyre is down to its carcass and has let
    /// go of its air.
    #[serde(default)]
    pub punctured: bool,
    /// A slow puncture: how fast the air is leaving, kPa/s, and how much
    /// has gone (`tyre_thermal::puncture`).
    #[serde(default)]
    pub leak_kpa_per_s: f32,
    /// Rubber picked up off the road, 0..1 (`tyre_thermal::step`): the
    /// marbles under the tyre stick to its tread and are flung off over a
    /// lap, a wet road washes them. Costs up to
    /// `tyre_thermal::PICKUP_GRIP_LOSS` of the tyre's grip.
    #[serde(default)]
    pub pickup: f32,
    /// Blistering, 0..1: how long the tread has run over its window (a
    /// minute a window over blisters it through). Looks only: no grip.
    #[serde(default)]
    pub blister: f32,
    #[serde(default)]
    pub pressure_loss_kpa: f32,
}

#[derive(Debug, Clone, Copy, Default, Serialize, Deserialize)]
pub struct TireTelemetry {
    pub front_left: TireData,
    pub front_right: TireData,
    pub rear_left: TireData,
    pub rear_right: TireData,
}

impl TireTelemetry {
    /// The four tyres, FL FR RL RR.
    pub fn each(&self) -> [&TireData; 4] {
        [
            &self.front_left,
            &self.front_right,
            &self.rear_left,
            &self.rear_right,
        ]
    }

    pub fn each_mut(&mut self) -> [&mut TireData; 4] {
        [
            &mut self.front_left,
            &mut self.front_right,
            &mut self.rear_left,
            &mut self.rear_right,
        ]
    }
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

/// Where along each body zone's face the hits landed, -1 to 1, weighted by
/// the damage each did: the nose and the tail across the car (positive to the
/// left), the left and right flanks along it (positive forward). 0 for a zone
/// not hit, or hit dead centre. The client dents the zone there instead of
/// across the whole face (`crate::damage::hit_zone`). A hit too far along the
/// face from the first dent starts a second one (`second`, front, rear, left,
/// right), which later hits near it fold into; `second_pct` is the damage it
/// carries, 0 for a zone with one dent.
#[derive(Debug, Clone, Copy, Default, PartialEq, Serialize, Deserialize)]
pub struct HitCentres {
    pub front: f32,
    pub rear: f32,
    pub left: f32,
    pub right: f32,
    #[serde(default)]
    pub second: [f32; 4],
    #[serde(default)]
    pub second_pct: [f32; 4],
}

#[derive(Debug, Clone, Copy, Default, Serialize, Deserialize)]
pub struct DamageState {
    pub front_damage_percent: f32,
    pub rear_damage_percent: f32,
    pub left_damage_percent: f32,
    pub right_damage_percent: f32,
    pub engine_damage_percent: f32,
    /// The part of `engine_damage_percent` that is wear (`damage::engine_wear`,
    /// the hours at racing revs), which no pit crew can undo: a repair takes
    /// the engine back to this, not to zero.
    #[serde(default)]
    pub engine_wear_percent: f32,
    pub is_drivable: bool,
    /// Where the zones were hit; see [`HitCentres`].
    #[serde(default)]
    pub hit_at: HitCentres,
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
    /// The driver drives the pit lane themselves: the pit autopilot never
    /// takes the car (`ClientMessage::SetDriverAids`'s `pit_autopilot`).
    #[serde(default)]
    pub manual_pits: bool,
    /// How much damage this car takes: the session's rule
    /// (`RaceSession::damage`), stamped on every car it seats, AI included;
    /// `None` (a car no session seated) is full damage. Read through
    /// [`CarState::damage_scale`].
    #[serde(default)]
    pub damage_level: Option<DamageLevel>,

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

    /// A qualifying car out on its outlap (`GameSession::hotlap_relocate`):
    /// it was put just past the line, and lap 1 starts when it next crosses
    /// it, not 10% of the way round as for any other car placed there.
    #[serde(default)]
    pub outlap: bool,

    /// Out of the race (`DamageState::is_drivable` false): how long it has
    /// stood where it stopped, s, and whether it has been towed away to its
    /// pit box since (`GameSession::update_retirements`). A towed car is
    /// out of the collision passes, the wake and the DRS gaps.
    #[serde(default)]
    pub retired_s: f32,
    #[serde(default)]
    pub towed: bool,
    /// A recovery under way (`crate::recovery`, `ClientMessage::RecoverCar`):
    /// the car is held where it was put, out of every collision, wake and
    /// DRS pass, until it is released.
    #[serde(default)]
    pub recovery: Option<crate::recovery::Recovery>,

    /// The drag reduction system (`crate::drs`). `drs_allowed`: the car is
    /// in an activation zone it earned at the detection point, so the
    /// driver may open the flap; `drs_open`: it is open this tick, and the
    /// aero is reduced by the car's `DrsSpec`. Telemetry carries both as
    /// bits 3 and 4 of `lap_flags`.
    #[serde(default)]
    pub drs_allowed: bool,
    #[serde(default)]
    pub drs_open: bool,
    /// The flags race control shows this car (`crate::race_control`): the
    /// worst yellow in its sector and the next (0 none, 1 single, 2
    /// double), and the blue flag (a car a lap up close behind). Set each
    /// racing tick; out of a race both are clear.
    #[serde(default)]
    pub flag_yellow: u8,
    #[serde(default)]
    pub flag_blue: bool,
    /// The revs the gearbox forces on the engine past its limit (a missed
    /// downshift), as a ratio of `max_engine_rpm` (`physics::geared_rpm`,
    /// not in top gear); 0 while it is within the limit. `engine_rpm`
    /// clamps at the limit, so this is where telemetry reads the over-rev
    /// (`over_rev_pct`, `ENGINE_FLAG_OVER_REV`).
    #[serde(default)]
    pub over_rev_ratio: f32,
    /// Seconds left of the cue that the box refused a downshift
    /// (`physics::protected_gear` held the gear or stopped short of the
    /// one asked for); 0 when none is showing.
    #[serde(default)]
    pub shift_refused_s: f32,
    /// The next gear down would spin the engine past its limit at this
    /// road speed: the cue not to pull the paddle (on a box without
    /// protection the shift would over-rev; with it, it would be refused).
    #[serde(default)]
    pub downshift_risk: bool,
    /// The pace race control holds the car to, m/s: a formation lap's
    /// (`crate::race_start`) or the virtual safety car's
    /// (`crate::race_control`); 0 for none. The physics holds a car to it as
    /// the pit limiter does, and the AI starts no pass under it.
    #[serde(default)]
    pub speed_cap_mps: f32,
    /// Where across the road race control has the car hold, m left of the
    /// centerline: its grid slot's side as a rolling start's field forms
    /// up two by two (`crate::race_start`). The AI drives it as a lane;
    /// `None` for none.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub formation_lane_m: Option<f32>,
    /// On a formation or pace lap, free to warm its tyres: the AI weaves on
    /// the straights (`crate::race_start`).
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub warm_up: bool,

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
    /// The tyres' temperatures have been set for this run
    /// (`tyre_thermal::fit`). A car nobody fitted gets its tyres at their
    /// optimum on its first tick.
    #[serde(default)]
    pub tyres_fitted: bool,
    /// The compound on the car, an index into its tyre's
    /// `TireConfig::compounds()` (without its own list
    /// `tyre_thermal::default_compounds()`: 0 soft, 1 medium, 2 hard, 3
    /// intermediate, 4 wet), fitted with the set.
    #[serde(default = "default_medium")]
    pub tyre_compound: u8,
    /// The car against the pit lane, and its stop (`crate::pit`).
    #[serde(default)]
    pub pit: crate::pit::PitState,
    /// Each corner's brake, °C, FL FR RL RR (`crate::brakes`), and how worn
    /// its pads are, percent.
    #[serde(default)]
    pub brake_temp_c: [f32; 4],
    #[serde(default)]
    pub brake_wear_pct: [f32; 4],
    /// How worn each disc is, percent, apart from its pads: only on a car
    /// with `brake_disc_wear_per_mj` (`crate::brakes`).
    #[serde(default)]
    pub brake_disc_wear_pct: [f32; 4],
    /// Whether the discs wear at all (`brake_disc_wear_per_mj` > 0): what
    /// tells telemetry's `disc_wear` from a car whose discs never do.
    #[serde(default)]
    pub brake_discs_wear: bool,
    /// The braking power the driven axle's tyres put down last tick, W:
    /// what the hybrid may recover under brake-by-wire (`crate::hybrid`).
    #[serde(default)]
    pub driven_axle_brake_w: f32,
    /// What brake-by-wire is taking off each axle's hydraulics for the
    /// recovery, N, front and rear: the recovery itself, or a lagging
    /// estimate of it on a car with `bbw_response_s` (`crate::hybrid`).
    #[serde(default)]
    pub bbw_offset_n: [f32; 2],
    /// The ground's gradient under the car in the world frame (rise per
    /// metre along world x, then y), filtered, and the vertical
    /// acceleration the road's curvature along the car's path gives the
    /// body (positive into the road: a dip loads the car, a crest unloads
    /// it; `physics::update_car_3d`).
    #[serde(default)]
    pub ground_slope_f: f32,
    #[serde(default)]
    pub ground_slope_y_f: f32,
    #[serde(default)]
    pub vertical_accel_mps2: f32,
    /// The floor's porpoising (`crate::aero`): the oscillation's phase, rad,
    /// and amplitude as a share of the downforce.
    #[serde(default)]
    pub porpoise_phase: f32,
    #[serde(default)]
    pub porpoise_amp: f32,
    /// Each wheel's suspension speed filtered over a few ticks, m/s: the
    /// strike measure kerb, bottoming and landing damage reads
    /// (`crate::damage`), which a one-tick step in the road mesh does not
    /// reach.
    #[serde(default)]
    pub strike_mps: [f32; 4],
    /// The air this car drives into: the tow and dirty air of the cars
    /// ahead (`crate::slipstream`), set each tick before the physics.
    #[serde(default)]
    pub wake: crate::slipstream::Wake,
    /// What an AI driver is doing about the cars around it: passing,
    /// defending, letting a car by (`crate::racecraft`), set each tick
    /// before the physics and read by the controller the next.
    #[serde(default)]
    pub racecraft: crate::racecraft::Racecraft,
    /// The tyres' load with the downforce the car makes now over the load
    /// it would have at this ground speed in still, clean air (the wind and
    /// the wake of a car ahead), never over 1: what the AI plans its grip
    /// by. Set by the physics each tick.
    #[serde(default = "default_one_f32")]
    pub aero_load_share: f32,
    /// The grip of the road under the car against the road the session
    /// was planned on (`crate::road_state`: rubber and a drying line give
    /// more, marbles and a puddle less), 1.0 on the road as baked. Set by the physics each tick; the AI
    /// reads it through `tyre_grip_share`.
    #[serde(default = "default_one_f32")]
    pub surface_grip_share: f32,
    /// What the fitted compound's bite on the snow and ice under the car
    /// adds against the tyre the road was baked for
    /// (`tyre_thermal::Compound::snow_factor`): about 2 for a studded snow
    /// tyre on a covered road, 1.0 for any other tyre or a bare road. Set
    /// by the physics each tick; the AI reads it through `tyre_grip_share`.
    #[serde(default = "default_one_f32")]
    pub snow_tyre_share: f32,
    /// The road under each tyre, FL FR RL RR, as the physics last sampled
    /// it (`crate::road_state`): the water in the weather's units and the
    /// marbles 0..1. Telemetry carries them per tyre, so a client draws
    /// each tyre's own water and pickup. Set by the physics each tick.
    #[serde(default)]
    pub tyre_water: [f32; 4],
    #[serde(default)]
    pub tyre_marbles: [f32; 4],
    /// Each wheel's slip this tick, FL FR RL RR, in multiples of the tyre's
    /// peak (what the driver feedback carries), and the surface under it
    /// (`feedback::ContactSurface` as a byte): the spectator stream's car
    /// row carries them, so a replay sounds its tyres and kerbs. Set by the
    /// physics each tick.
    #[serde(default)]
    pub wheel_slip_ratio: [f32; 4],
    #[serde(default)]
    pub wheel_slip_angle: [f32; 4],
    #[serde(default)]
    pub wheel_surface: [u8; 4],
    /// The soft ground under each tyre that is off the track
    /// (`physics::OffGround` as `network::GROUND_*`: 0 grass or not off,
    /// 1 gravel, 2 sand, 3 the desert floor), for the clients' dust, the
    /// road sound and the force feedback. Set by the physics each tick.
    #[serde(default)]
    pub wheel_ground: [u8; 4],
    /// The hardest hit this car took since the pass that read it, percent
    /// of damage (`physics::apply_damage_to_car`): what sheds debris and
    /// what may puncture a tyre (`GameSession::update_debris`).
    #[serde(default)]
    pub last_hit_pct: f32,
    /// Which way the last hit came from, rad in the car's frame (0 the
    /// nose, π/2 the left side), for the tyre nearest it.
    #[serde(default)]
    pub last_hit_angle: f32,
    /// The road state's cell the car was last in (`GameSession::update_road`).
    #[serde(default)]
    pub road_cell: u32,
    /// What the car's tyres did on the road since it last crossed into a
    /// cell, and where their patches run (`crate::road_state::RoadWork`):
    /// added up by the physics, laid as its passes by `update_road`.
    #[serde(skip)]
    pub road_work: crate::road_state::RoadWork,
    /// Which tyres are sliding hard (bits 0-3) and locked (bits 4-7), for
    /// the client's smoke (`network::CompactCarState::slide_flags`).
    #[serde(default)]
    pub slide_flags: u8,
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
    /// The hybrid's mode (`crate::hybrid::ErsMode` as a byte), the
    /// overtake button, what the motor deployed this lap (kJ) and on which
    /// lap that count started, the car's share of the lap for the pacing,
    /// and what the motor did this tick.
    #[serde(default = "default_ers_mode")]
    pub ers_mode: u8,
    #[serde(default)]
    pub ers_boost: bool,
    #[serde(default)]
    pub ers_deployed_kj: f32,
    #[serde(default)]
    pub ers_lap_mark: u16,
    #[serde(default)]
    pub ers_lap_share: f32,
    #[serde(default)]
    pub ers_deploying: bool,
    #[serde(default)]
    pub ers_harvesting: bool,
    /// What telemetry says of the hybrid, kept by `crate::hybrid`: the
    /// charge and the lap budget left, percent, 255 for none.
    #[serde(default = "no_hybrid_pct")]
    pub ers_charge_pct: u8,
    #[serde(default = "no_hybrid_pct")]
    pub ers_budget_pct: u8,
    /// What the motor has deployed this stint (kJ), what the overtake
    /// button has spent this lap (kJ), and the stint's budget left for
    /// telemetry, percent (255: no stint limit).
    #[serde(default)]
    pub ers_stint_kj: f32,
    /// The whole car's energy spent this stint, MJ, fuel and motor, on a
    /// car with `[fuel] stint_energy_mj` (`FuelConfig::stint_left_mj`).
    #[serde(default)]
    pub stint_energy_used_mj: f32,
    /// The stint's energy left, percent, for telemetry (255: no
    /// `stint_energy_mj`); `FuelConfig::stint_pct`, kept by the fuel step.
    #[serde(default = "no_hybrid_pct")]
    pub stint_energy_pct: u8,
    #[serde(default)]
    pub ers_override_kj: f32,
    #[serde(default = "no_hybrid_pct")]
    pub ers_stint_pct: u8,

    /// Turbo spool, 0..1: how much of the boost the turbo is delivering
    /// (`TurboConfig`). Stays 0 on a car without one.
    #[serde(default)]
    pub turbo_spool: f32,

    // Aerodynamics
    pub downforce_front_n: f32,
    pub downforce_rear_n: f32,
    pub drag_force_n: f32,
    /// Where each axle rides, m, as the aero map read it this tick
    /// (`crate::aero::ride_heights`); 0 before the first tick.
    #[serde(default)]
    pub ride_height_front_m: f32,
    #[serde(default)]
    pub ride_height_rear_m: f32,

    /// What the driver should feel, collected each tick for the next
    /// `DriverFeedback` message. Output only: nothing in the sim reads it.
    #[serde(skip)]
    pub feedback: crate::feedback::FeedbackAccumulator,
}

impl CarState {
    /// Out of everyone's way: towed after retiring, or held by a recovery.
    /// Other cars, walls, the wake and the DRS gaps pass through it.
    pub fn is_ghost(&self) -> bool {
        self.towed || self.recovery.is_some()
    }

    /// The aids this car runs, as a `DriverAids`.
    pub fn driver_aids(&self) -> DriverAids {
        DriverAids {
            auto_gearbox: self.auto_gearbox,
            steering_assist: self.steering_assist,
            abs: self.abs,
            traction_control: self.traction_control,
            pit_autopilot: Some(!self.manual_pits),
        }
    }

    /// What every damage accrual to this car's body is multiplied by.
    pub fn damage_scale(&self) -> f32 {
        self.damage_level.unwrap_or_default().scale()
    }

    /// What every damage accrual to this car's engine is multiplied by.
    pub fn engine_damage_scale(&self) -> f32 {
        self.damage_level.unwrap_or_default().engine_scale()
    }

    /// The damage the physics, the pit crew and the AI's strategy act on:
    /// the car's own, or none under `DamageLevel::VisualOnly`, where the
    /// dents are only drawn.
    pub fn felt_damage(&self) -> DamageState {
        if self.damage_level.unwrap_or_default().costs_performance() {
            self.damage
        } else {
            DamageState {
                is_drivable: true,
                ..Default::default()
            }
        }
    }

    /// Under `DamageLevel::VisualOnly` a zone at 100% does not put the car
    /// out: called after every accrual.
    pub fn keep_visual_damage_drivable(&mut self) {
        if !self.damage_level.unwrap_or_default().costs_performance() {
            self.damage.is_drivable = true;
        }
    }

    /// Set the aids; the automatic box starts its timers afresh.
    pub fn set_driver_aids(&mut self, aids: DriverAids) {
        if let Some(restore) = self.pit.restore_aids.as_mut() {
            // The pit autopilot holds the gearbox and the steering aid: the
            // driver's new choice is what it hands back.
            *restore = [aids.auto_gearbox, aids.steering_assist];
        } else {
            self.auto_gearbox = aids.auto_gearbox;
            self.auto_shift_hold_ticks = 0;
            self.auto_reverse_ticks = 0;
            self.steering_assist = aids.steering_assist;
        }
        self.abs = aids.abs;
        self.traction_control = aids.traction_control;
        self.manual_pits = aids.pit_autopilot == Some(false);
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
            manual_pits: false,
            damage_level: None,

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
            outlap: false,
            retired_s: 0.0,
            towed: false,
            recovery: None,
            drs_allowed: false,
            flag_yellow: 0,
            flag_blue: false,
            over_rev_ratio: 0.0,
            shift_refused_s: 0.0,
            downshift_risk: false,
            speed_cap_mps: 0.0,
            formation_lane_m: None,
            warm_up: false,
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
            tyres_fitted: false,
            tyre_compound: crate::tyre_thermal::MEDIUM,
            pit: crate::pit::PitState::default(),
            brake_temp_c: [20.0; 4],
            brake_wear_pct: [0.0; 4],
            brake_disc_wear_pct: [0.0; 4],
            brake_discs_wear: false,
            driven_axle_brake_w: 0.0,
            bbw_offset_n: [0.0; 2],
            ground_slope_f: 0.0,
            ground_slope_y_f: 0.0,
            vertical_accel_mps2: 0.0,
            porpoise_phase: 0.0,
            porpoise_amp: 0.0,
            strike_mps: [0.0; 4],
            wake: crate::slipstream::Wake::CLEAN,
            racecraft: crate::racecraft::Racecraft::default(),
            aero_load_share: 1.0,
            surface_grip_share: 1.0,
            snow_tyre_share: 1.0,
            tyre_water: [0.0; 4],

            tyre_marbles: [0.0; 4],
            wheel_slip_ratio: [0.0; 4],
            wheel_slip_angle: [0.0; 4],
            wheel_surface: [0; 4],
            wheel_ground: [0; 4],
            last_hit_pct: 0.0,
            last_hit_angle: 0.0,
            road_cell: 0,
            road_work: Default::default(),
            slide_flags: 0,
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
            water_temp_c: crate::engine_heat::START_C,

            // Weight (will be calculated)
            weight_front_left_n: 0.0,
            weight_front_right_n: 0.0,
            weight_rear_left_n: 0.0,
            weight_rear_right_n: 0.0,

            wheel_angular_vel: [0.0; 4],
            hybrid_battery_kwh: default_battery_uninitialized(),
            ers_mode: default_ers_mode(),
            ers_boost: false,
            ers_deployed_kj: 0.0,
            ers_lap_mark: 0,
            ers_lap_share: 0.0,
            ers_deploying: false,
            ers_harvesting: false,
            ers_charge_pct: 255,
            ers_budget_pct: 255,
            ers_stint_kj: 0.0,
            stint_energy_used_mj: 0.0,
            stint_energy_pct: 255,
            ers_override_kj: 0.0,
            ers_stint_pct: 255,
            turbo_spool: 0.0,

            // Aerodynamics (will be calculated)
            downforce_front_n: 0.0,
            downforce_rear_n: 0.0,
            drag_force_n: 0.0,
            ride_height_front_m: 0.0,
            ride_height_rear_m: 0.0,

            feedback: Default::default(),
        }
    }
}

fn no_hybrid_pct() -> u8 {
    255
}

fn default_ers_mode() -> u8 {
    crate::hybrid::DEFAULT_MODE
}

fn default_battery_uninitialized() -> f32 {
    -1.0
}

fn default_traction_control() -> bool {
    true
}

/// The AWD split every car had before it was a key: 40% to the front.
pub const DEFAULT_AWD_FRONT_SHARE: f32 = 0.4;

fn default_awd_front_share() -> f32 {
    DEFAULT_AWD_FRONT_SHARE
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
    // 2 was Sandbox, which had no behaviour; never reuse it.
    /// An AI-only race nobody drives: what a showcase (the menu's backdrop)
    /// is rendered from. A server refuses to create one; it stays on the
    /// wire so older streams and clients decode.
    Demo = 3,
    /// One AI car lapping the track alone, on the line and at the speed its
    /// car can manage (the menu's "Watch hotlap"), simulated by the game
    /// itself (`hotlap_watch::LocalWatch`) for the car, setup and sky the
    /// player picked. A server refuses to create one.
    HotlapWatch = 4,
}

impl SessionKind {
    /// A session nobody drives, which the game simulates for itself and a
    /// server refuses to create.
    pub fn is_watch_only(self) -> bool {
        matches!(self, SessionKind::Demo | SessionKind::HotlapWatch)
    }
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

/// How much damage the cars in a session take: a rule of the session, chosen
/// by its host on create (`ClientMessage::CreateSession::damage`) and the same
/// for every car in it, AI included (`RaceSession::damage`). It scales every
/// accrual in `crate::damage` (impacts, overheating, over-revving); what
/// damage already done costs is the same whichever level is set, except
/// under `VisualOnly`, where it costs nothing. Encoded as a small integer.
#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize_repr, Deserialize_repr, Default)]
pub enum DamageLevel {
    /// The car takes no damage at all.
    Off = 0,
    /// Every hit and every overheating second does [`DamageLevel::REDUCED_SHARE`]
    /// of what it would.
    Reduced = 1,
    /// Damage as the model has it.
    #[default]
    Full = 2,
    /// The body takes every dent and loses its parts as under `Full`, so
    /// the client draws the crash, but nothing costs the car anything: no
    /// grip, downforce, cooling or power lost, no engine damage, no
    /// puncture or debris from a hit, never out of the race
    /// (`CarState::felt_damage`).
    VisualOnly = 3,
}

impl DamageLevel {
    /// The share of the damage a `Reduced` car takes.
    pub const REDUCED_SHARE: f32 = 0.5;

    /// Full damage, the default: left off the wire, so every message from
    /// before the session rule keeps its bytes.
    pub fn is_full(&self) -> bool {
        *self == DamageLevel::Full
    }

    /// What this level multiplies every damage accrual to the body by.
    pub fn scale(self) -> f32 {
        match self {
            DamageLevel::Off => 0.0,
            DamageLevel::Reduced => Self::REDUCED_SHARE,
            DamageLevel::Full | DamageLevel::VisualOnly => 1.0,
        }
    }

    /// What this level multiplies the engine's damage by (heat, a missed
    /// shift, wear, the share of a hit to the nose): nothing to see, so
    /// none under `VisualOnly`.
    pub fn engine_scale(self) -> f32 {
        match self {
            DamageLevel::VisualOnly => 0.0,
            other => other.scale(),
        }
    }

    /// Whether the damage done costs the car anything.
    pub fn costs_performance(self) -> bool {
        self != DamageLevel::VisualOnly
    }
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
    /// The pit autopilot: `Some(false)` leaves the pit lane to the driver
    /// (the limiter still holds the limit); `None` keeps it on.
    pub pit_autopilot: Option<bool>,
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
            // Driving the lane yourself is never an advantage to forbid.
            pit_autopilot: asked.pit_autopilot,
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
    /// Snow falling on a cold road (`crate::road_state`'s snow cover).
    Snow = 5,
}

impl Weather {
    pub const ALL: [Weather; 6] = [
        Weather::Sunny,
        Weather::Cloudy,
        Weather::Overcast,
        Weather::LightRain,
        Weather::HeavyRain,
        Weather::Snow,
    ];

    /// The air a snow sky sits under its overcast sibling, °C: the same
    /// swing over the day, 17 degrees colder.
    pub const SNOW_AIR_DROP_C: f32 = 17.0;
    /// The warmest air a host may pick under snow, °C.
    pub const SNOW_MAX_AIR_C: i8 = 2;

    pub fn from_u8(value: u8) -> Option<Weather> {
        Weather::ALL.get(value as usize).copied()
    }

    /// Whether rain is falling.
    pub fn is_wet(self) -> bool {
        matches!(self, Weather::LightRain | Weather::HeavyRain)
    }

    pub fn is_snow(self) -> bool {
        self == Weather::Snow
    }

    /// Rain falling, in the weather's water units (`water` less the slush a
    /// snow sky keeps on the road).
    pub fn rainfall(self) -> f32 {
        if self.is_wet() {
            self.water()
        } else {
            0.0
        }
    }

    /// Snow falling, 0 or 1 (1 is steady snowfall, the road's cover builds
    /// from it: `road_state::SNOWFALL_RATE`).
    pub fn snowfall(self) -> f32 {
        if self.is_snow() {
            1.0
        } else {
            0.0
        }
    }

    /// What the asphalt's grip is multiplied by. Dry weather leaves the
    /// track as filed; a cold overcast track is a shade slower; rain takes
    /// the field down to where a wet race sits, some 15% off a dry lap, on
    /// the tyre the weather calls for (inters in light rain, wets in heavy:
    /// any other tyre loses more, `tyre_thermal::Compound::grip_on`).
    pub fn road_grip_factor(self) -> f32 {
        match self {
            Weather::Sunny | Weather::Cloudy => 1.0,
            Weather::Overcast => 0.98,
            Weather::LightRain => 0.86,
            Weather::HeavyRain => 0.74,
            // The baked road is a snow-covered one, a quarter of the dry
            // grip and more on cold tyres: the racing line and the AI's
            // speed profile are built for it, the cars crawl from lap one
            // (`road_state::snow_grip` clears a line) and the field does
            // not race it cleanly.
            Weather::Snow => 1.0 - crate::road_state::SNOW_COVER_GRIP,
        }
    }

    /// Water on the road, 0 dry to 1: light rain is half of heavy. Which
    /// tyre grips is read off it (`tyre_thermal::Compound::grip_on`).
    pub fn water(self) -> f32 {
        match self {
            Weather::LightRain => 0.5,
            Weather::HeavyRain => 1.0,
            // The slush on the road: a treaded tyre's territory, not the
            // full wet's.
            Weather::Snow => crate::road_state::SNOW_WATER,
            _ => 0.0,
        }
    }

    /// Curbs are painted, and paint in the wet is close to ice: a further
    /// cut on top of `road_grip_factor`.
    pub fn curb_grip_factor(self) -> f32 {
        match self {
            Weather::LightRain => 0.85,
            Weather::HeavyRain => 0.75,
            Weather::Snow => 1.0,
            _ => 1.0,
        }
    }

    /// Wet grass and gravel, on top of `road_grip_factor` (a snowy verge
    /// is a wall of slush).
    pub fn off_track_grip_factor(self) -> f32 {
        match self {
            Weather::LightRain => 0.85,
            Weather::HeavyRain => 0.7,
            Weather::Snow => 1.0,
            _ => 1.0,
        }
    }

    /// How much of the sky the weather's cloud covers, 0..1.
    pub fn cloud(self) -> f32 {
        match self {
            Weather::Sunny => 0.0,
            Weather::Cloudy => 0.5,
            Weather::Overcast => 0.85,
            Weather::LightRain => 0.9,
            Weather::HeavyRain => 1.0,
            Weather::Snow => 1.0,
        }
    }

    pub fn name(self) -> &'static str {
        match self {
            Weather::Sunny => "sunny",
            Weather::Cloudy => "cloudy",
            Weather::Overcast => "overcast",
            Weather::LightRain => "light rain",
            Weather::HeavyRain => "heavy rain",
            Weather::Snow => "snow",
        }
    }
}

/// Weather and the clock, per session. Carried inside `CreateSession`,
/// echoed in `SessionJoined` and listed in every `SessionSummary`. Absent
/// fields (a client or server from before the feature) mean a sunny early
/// afternoon, which is what every session was until now.
///
/// The air (temperature, humidity, wind) is optional: `None` is "from the
/// weather and the clock", which the server works out when it creates the
/// session ([`SessionConditions::resolve`]), so what a session echoes and
/// lists always names every figure. An absent field is left off the wire.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub struct SessionConditions {
    #[serde(default)]
    pub weather: Weather,
    /// Local time of day, minutes after midnight (0..=1439). The sun it
    /// puts up warms the air and the asphalt; the client lights by it.
    #[serde(default = "SessionConditions::default_time_of_day")]
    pub time_of_day_minutes: u16,
    /// Air temperature, °C.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub air_temp_c: Option<i8>,
    /// Relative humidity, percent.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub humidity_pct: Option<u8>,
    /// Mean wind speed, km/h (it gusts about that).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub wind_kph: Option<u8>,
    /// Where the wind blows *from*, degrees from the start straight's
    /// direction: 0 is head-on down the straight, 90 from its left.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub wind_from_deg: Option<u16>,
    /// How fast the day's clock runs against the session's: 0 (or absent)
    /// holds it at `time_of_day_minutes` for the session's life, 1 is real
    /// time, 24 a day in an hour (`crate::conditions`).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub time_scale: Option<u8>,
    /// How changeable the weather is: 0 (or absent) holds the host's sky,
    /// 1 to 3 (settled, changeable, stormy) let it follow a forecast worked
    /// out from the session's id (`crate::conditions::forecast`).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub changeable: Option<u8>,
    /// The rubber on the racing line at the start, percent: 0 a green
    /// track, 50 (or absent) what every car is calibrated on, 100 a track
    /// rubbered in by a weekend of racing (`crate::road_state`).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub track_rubber_pct: Option<u8>,
    /// The seed the forecast and the wind's direction are worked out from
    /// (`crate::conditions::forecast`, `resolve`): the session's id when
    /// absent. A host sets one to see the forecast before the session
    /// exists (the create screen shows it, and can roll another), and a
    /// replay or a test to get a known sky back.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub sky_seed: Option<u64>,
    /// The date, day of the year 1..=365 (1 January is 1): how high the
    /// sun climbs and how long the day is (`sun_declination_deg`), and
    /// whether the circuit keeps summer time. Absent, the model day: late
    /// May ([`SUN_DECLINATION_DEG`], [`MODEL_DAY_OF_YEAR`]).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub day_of_year: Option<u16>,
    /// Solar noon on the session's clock, minutes after midnight: the
    /// server's to name, from where the circuit is
    /// ([`SessionConditions::at_site`]: its longitude, its time zone,
    /// summer time and the equation of time on the date). Absent (a track
    /// that does not say where it is, or an older server), 13:00
    /// ([`SOLAR_NOON_HOURS`]). A create's own is replaced.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub solar_noon_min: Option<u16>,
    /// The host's own forecast: the weather from each step's minute of
    /// session time on (`HostForecast`), used instead of the one worked
    /// out from the seed (`crate::conditions::forecast`), whatever
    /// `changeable` says. Empty (and left off the wire) for none.
    #[serde(default, skip_serializing_if = "HostForecast::is_empty")]
    pub forecast: HostForecast,
}

/// One step of a host's forecast: from `at_min` minutes into the session
/// the weather is `weather`. On the wire a two-element array.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ForecastStep {
    pub at_min: u16,
    pub weather: Weather,
}

/// A forecast the host wrote (`SessionConditions::forecast`): up to
/// [`HostForecast::MAX_STEPS`] steps in time order, each a change of
/// weather at least [`HostForecast::MIN_GAP_MIN`] after the last (and after
/// the start), none past [`HostForecast::HORIZON_MIN`]. Held in a fixed
/// array of packed steps (the minute over three bits of weather) so the
/// conditions stay `Copy` and small. Serialized as an array of
/// `[at_min, weather]` pairs; [`HostForecast::from_steps`] makes any list
/// one: sorted, steps too soon after the last or repeating its weather
/// dropped, an unknown weather byte dropped, the rest cut at the limit.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct HostForecast {
    len: u8,
    packed: [u16; HostForecast::MAX_STEPS],
}

impl Default for HostForecast {
    fn default() -> Self {
        Self::NONE
    }
}

impl HostForecast {
    /// The most steps a host may write.
    pub const MAX_STEPS: usize = 10;
    /// A step comes at least this long after the last, and the first this
    /// long after the start, minutes: rain takes minutes to arrive (the
    /// generated forecast's own gap, `conditions::forecast`).
    pub const MIN_GAP_MIN: u16 = 4;
    /// No step later than this, minutes of session: a 24-hour race and its
    /// run-up.
    pub const HORIZON_MIN: u16 = 26 * 60;
    pub const NONE: HostForecast = HostForecast {
        len: 0,
        packed: [0; HostForecast::MAX_STEPS],
    };

    /// Adds `step` at the end, if there is room.
    fn push(&mut self, step: ForecastStep) {
        if (self.len as usize) < Self::MAX_STEPS {
            let at_min = step.at_min.min(Self::HORIZON_MIN + 1);
            self.packed[self.len as usize] = (at_min << 3) | step.weather as u16;
            self.len += 1;
        }
    }

    /// A forecast from `steps` as a host wrote them, made valid (see the
    /// type), starting from the session's own `start` weather (a first
    /// step repeating it is dropped).
    pub fn from_steps(start: Weather, steps: &[ForecastStep]) -> Self {
        let mut sorted: Vec<ForecastStep> = steps.to_vec();
        sorted.sort_by_key(|s| s.at_min);
        let mut out = Self::NONE;
        let (mut last_min, mut last_weather) = (0u16, start);
        for step in sorted {
            if out.len as usize == Self::MAX_STEPS || step.at_min > Self::HORIZON_MIN {
                break;
            }
            if step.at_min < last_min.saturating_add(Self::MIN_GAP_MIN)
                || step.weather == last_weather
            {
                continue;
            }
            out.push(step);
            (last_min, last_weather) = (step.at_min, step.weather);
        }
        out
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    /// The steps, in time order.
    pub fn steps(&self) -> Vec<ForecastStep> {
        self.packed[..self.len as usize]
            .iter()
            .map(|p| ForecastStep {
                at_min: p >> 3,
                weather: Weather::from_u8((p & 7) as u8).unwrap_or_default(),
            })
            .collect()
    }

    /// The same forecast with each step's weather passed through `f` and
    /// made valid again from `start`.
    pub fn map(&self, start: Weather, f: impl Fn(Weather) -> Weather) -> Self {
        let mapped: Vec<ForecastStep> = self
            .steps()
            .iter()
            .map(|s| ForecastStep {
                at_min: s.at_min,
                weather: f(s.weather),
            })
            .collect();
        Self::from_steps(start, &mapped)
    }
}

impl Serialize for HostForecast {
    fn serialize<S: serde::Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        use serde::ser::SerializeSeq;
        let mut seq = serializer.serialize_seq(Some(self.len as usize))?;
        for step in &self.steps() {
            seq.serialize_element(&(step.at_min, step.weather))?;
        }
        seq.end()
    }
}

impl<'de> Deserialize<'de> for HostForecast {
    fn deserialize<D: serde::Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        // Anything a host may send: unknown weather dropped, the steps in
        // time order and cut at the limit. `SessionConditions::clamp` makes
        // the rest good from the start's weather (`from_steps`).
        let raw: Vec<(u64, u64)> = Vec::deserialize(deserializer)?;
        let mut steps: Vec<ForecastStep> = raw
            .into_iter()
            .filter_map(|(at_min, weather)| {
                let weather = Weather::from_u8(u8::try_from(weather).ok()?)?;
                let at_min = at_min.min(Self::HORIZON_MIN as u64 + 1) as u16;
                Some(ForecastStep { at_min, weather })
            })
            .collect();
        steps.sort_by_key(|s| s.at_min);
        let mut out = Self::NONE;
        for step in steps.iter().take(Self::MAX_STEPS) {
            out.push(*step);
        }
        Ok(out)
    }
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
        air_temp_c: None,
        humidity_pct: None,
        wind_kph: None,
        wind_from_deg: None,
        time_scale: None,
        changeable: None,
        track_rubber_pct: None,
        sky_seed: None,
        day_of_year: None,
        solar_noon_min: None,
        forecast: HostForecast::NONE,
    };
    pub const MINUTES_PER_DAY: u16 = 24 * 60;

    fn default_time_of_day() -> u16 {
        Self::DEFAULT.time_of_day_minutes
    }

    /// Coldest and hottest air a host may pick, °C.
    pub const AIR_TEMP_RANGE_C: (i8, i8) = (-5, 45);
    /// Strongest wind a host may pick, km/h.
    pub const MAX_WIND_KPH: u8 = 60;
    /// Fastest a host may run the day's clock.
    pub const MAX_TIME_SCALE: u8 = 60;
    /// Most changeable a host may make the weather.
    pub const MAX_CHANGEABLE: u8 = 3;

    /// The same conditions with the clock wrapped onto one day and every
    /// figure held to what the sim takes.
    pub fn clamp(self) -> Self {
        let (cold, hot) = Self::AIR_TEMP_RANGE_C;
        Self {
            weather: self.weather,
            time_of_day_minutes: self.time_of_day_minutes % Self::MINUTES_PER_DAY,
            air_temp_c: self.air_temp_c.map(|t| {
                let hot = if self.weather.is_snow() {
                    hot.min(Weather::SNOW_MAX_AIR_C)
                } else {
                    hot
                };
                t.clamp(cold, hot)
            }),
            humidity_pct: self.humidity_pct.map(|h| h.min(100)),
            wind_kph: self.wind_kph.map(|w| w.min(Self::MAX_WIND_KPH)),
            wind_from_deg: self.wind_from_deg.map(|d| d % 360),
            time_scale: self.time_scale.map(|t| t.min(Self::MAX_TIME_SCALE)),
            changeable: self.changeable.map(|c| c.min(Self::MAX_CHANGEABLE)),
            track_rubber_pct: self.track_rubber_pct.map(|r| r.min(100)),
            sky_seed: self.sky_seed,
            day_of_year: self.day_of_year.map(|d| d.clamp(1, DAYS_PER_YEAR)),
            solar_noon_min: self.solar_noon_min.map(|m| m % Self::MINUTES_PER_DAY),
            forecast: HostForecast::from_steps(self.weather, &self.forecast.steps()),
        }
    }

    /// These conditions over `track`: solar noon named from where the
    /// circuit is (its `metadata.longitude_deg` and latitude,
    /// [`solar_noon_minutes`]), or none (13:00) for a track that does not
    /// say. The server calls it on create, after `resolve`, so the join
    /// tells the client where the sun stands.
    pub fn at_site(self, track: &TrackConfig) -> Self {
        let longitude = track.metadata.longitude_deg.filter(|l| l.is_finite());
        Self {
            solar_noon_min: longitude
                .map(|lon| solar_noon_minutes(track.latitude_deg(), lon, self.day())),
            ..self
        }
    }

    /// The day of the year the sky is worked out for.
    pub fn day(&self) -> u16 {
        self.day_of_year
            .map_or(MODEL_DAY_OF_YEAR, |d| d.clamp(1, DAYS_PER_YEAR))
    }

    /// Solar noon on the session's clock, hours.
    pub fn solar_noon_hours(&self) -> f32 {
        self.solar_noon_min.map_or(SOLAR_NOON_HOURS, |m| {
            (m % Self::MINUTES_PER_DAY) as f32 / 60.0
        })
    }

    /// The sun's declination on the session's date, degrees: the model
    /// day's [`SUN_DECLINATION_DEG`] exactly when no date is named.
    pub fn sun_declination_deg(&self) -> f32 {
        match self.day_of_year {
            None => SUN_DECLINATION_DEG,
            Some(_) => sun_declination_deg(self.day()),
        }
    }

    /// The sun's elevation, degrees, at `minutes` on this session's day
    /// over a circuit `latitude_deg` north.
    pub fn sun_elevation_at(&self, latitude_deg: f32, minutes: f32) -> f32 {
        sun_elevation_deg(
            latitude_deg,
            minutes,
            self.solar_noon_hours(),
            self.sun_declination_deg(),
        )
    }

    /// The seed the sky is laid out from: the host's `sky_seed`, else the
    /// session's (`session_seed`, the first half of its id).
    pub fn sky_seed_or(&self, session_seed: u64) -> u64 {
        self.sky_seed.unwrap_or(session_seed)
    }

    /// The day's clock speed, 0 when it stands still.
    pub fn clock_scale(&self) -> u8 {
        self.time_scale.unwrap_or(0)
    }

    /// The forecast's level, 0 when the sky holds.
    pub fn changeability(&self) -> u8 {
        self.changeable.unwrap_or(0)
    }

    /// Whether anything about the sky moves through the session.
    pub fn is_static(&self) -> bool {
        self.clock_scale() == 0 && self.changeability() == 0 && self.forecast.is_empty()
    }

    /// The racing line's rubber at the start, 0..1.
    pub fn track_rubber(&self) -> f32 {
        self.track_rubber_pct
            .map_or(crate::road_state::RUBBER_REFERENCE, |r| {
                r.min(100) as f32 / 100.0
            })
    }

    /// Every figure named: what was picked, and the weather's own for the
    /// rest, at [`DEFAULT_LATITUDE_DEG`] ([`Self::resolve_at`]).
    pub fn resolve(self, seed: u64) -> Self {
        self.resolve_at(seed, DEFAULT_LATITUDE_DEG)
    }

    /// Every figure named over a circuit at `site` (its latitude, or a
    /// [`ClimateSite`]: `TrackConfig::climate_site`): what was picked, and
    /// the weather's own for the rest, in the climate and season of the
    /// date there ([`Self::auto_air_temperature_c_at`],
    /// [`Self::auto_humidity_pct_at`]). `seed` picks an unnamed wind's
    /// direction (the session's id, so it is the same for everyone and for
    /// the life of the session).
    pub fn resolve_at(self, seed: u64, site: impl Into<ClimateSite>) -> Self {
        let site = site.into();
        let c = self.clamp();
        // The weather's own air is held to what a host may pick, so a
        // resolved session clamps to itself.
        let (cold, hot) = Self::AIR_TEMP_RANGE_C;
        let named = Self {
            air_temp_c: Some(
                c.air_temp_c.unwrap_or(
                    c.auto_air_temperature_c_at(site)
                        .round()
                        .clamp(cold as f32, hot as f32) as i8,
                ),
            ),
            humidity_pct: Some(c.humidity_pct.unwrap_or(c.auto_humidity_pct_at(site))),
            wind_kph: Some(c.wind_kph.unwrap_or(c.auto_wind_kph())),
            wind_from_deg: Some(
                c.wind_from_deg
                    .unwrap_or((crate::wind::hash01(seed, 0x57_1D) * 360.0) as u16 % 360),
            ),
            ..c
        };
        // A host's forecast keeps to its sky: a winter sky's rain falls as
        // snow, a summer sky's snow as light rain (the generated forecast's
        // own rule, `crate::conditions::is_winter`).
        let winter = crate::conditions::is_winter(&named);
        Self {
            forecast: named.forecast.map(named.weather, |w| match w {
                Weather::LightRain | Weather::HeavyRain if winter => Weather::Snow,
                Weather::Snow if !winter => Weather::LightRain,
                w => w,
            }),
            ..named
        }
    }

    /// The weather's humidity, percent.
    pub fn auto_humidity_pct(&self) -> u8 {
        match self.weather {
            Weather::Sunny => 50,
            Weather::Cloudy => 60,
            Weather::Overcast => 70,
            Weather::LightRain => 90,
            Weather::HeavyRain => 97,
            Weather::Snow => 85,
        }
    }

    /// How much warmer the season of the date makes the air at a site
    /// than the model day there, °C ([`crate::climate::season_offset_c`]:
    /// [`season_air_c`] by its climate's swing): exactly 0 when no date is
    /// named. What the humidity reads.
    pub fn season_offset_c(&self, site: impl Into<ClimateSite>) -> f32 {
        match self.day_of_year {
            None => 0.0,
            Some(_) => crate::climate::season_offset_c(self.day(), &site.into()),
        }
    }

    /// How much warmer the date at a site is than the reference day the
    /// weather's own air is filed at (late May, 50° N, on the sea), °C
    /// ([`crate::climate::climate_offset_c`]: the latitude's mean, the
    /// region's climate, the season, the lapse with height): exactly 0
    /// when no date is named, so a session without one is the late-May day
    /// it always was, wherever it is.
    pub fn climate_offset_c(&self, site: impl Into<ClimateSite>) -> f32 {
        match self.day_of_year {
            None => 0.0,
            Some(_) => crate::climate::climate_offset_c(self.day(), &site.into()),
        }
    }

    /// The weather's humidity in the season of the date, percent: a cold
    /// month's air holds less water and stands nearer saturation, so it is
    /// [`SEASON_HUMIDITY_PER_C`] a degree damper (drier in a hot one), and
    /// the tropics' wet season damper again
    /// ([`crate::climate::wet_season_pct`]). The model day's exactly when
    /// no date is named; snow's own 85% in any.
    pub fn auto_humidity_pct_at(&self, site: impl Into<ClimateSite>) -> u8 {
        let site = site.into();
        let own = self.auto_humidity_pct();
        let season = self.season_offset_c(site);
        let wet = match self.day_of_year {
            None => 0.0,
            Some(_) => crate::climate::wet_season_pct(self.day(), &site),
        };
        if (season == 0.0 && wet == 0.0) || self.weather.is_snow() {
            return own;
        }
        (own as f32 - SEASON_HUMIDITY_PER_C * season + wet)
            .round()
            .clamp(SEASON_HUMIDITY_MIN_PCT, 100.0) as u8
    }

    /// The weather's wind, km/h: a breeze in the sun, a blow in a storm.
    pub fn auto_wind_kph(&self) -> u8 {
        match self.weather {
            Weather::Sunny => 8,
            Weather::Cloudy => 12,
            Weather::Overcast => 15,
            Weather::LightRain => 18,
            Weather::HeavyRain => 28,
            Weather::Snow => 10,
        }
    }

    /// Relative humidity, 0..1.
    pub fn humidity(&self) -> f32 {
        self.humidity_pct.unwrap_or(self.auto_humidity_pct()) as f32 / 100.0
    }

    /// The wind the air moves with, m/s, *toward* where it blows (the
    /// opposite of `wind_from_deg`), in the start straight's frame: x down
    /// the straight, y to its left (`apply_to_track` turns it onto the
    /// track).
    pub fn wind_mps(&self) -> [f32; 2] {
        let speed = self.wind_kph.unwrap_or(self.auto_wind_kph()) as f32 / 3.6;
        let from = (self.wind_from_deg.unwrap_or(0) as f32).to_radians();
        [-speed * from.cos(), -speed * from.sin()]
    }

    /// The air's density, kg/m³, `altitude_m` above sea level at these
    /// conditions: the standard atmosphere's pressure for the height, the
    /// dry air and the water vapour each by their gas constants.
    pub fn air_density(&self, altitude_m: f32) -> f32 {
        air_density(altitude_m, self.air_temperature_c(), self.humidity())
    }

    /// The density against the reference day every car.toml is filed at
    /// (sea level, a sunny 13:00: [`SessionConditions::DEFAULT`]), which is
    /// what the aero and the engine are scaled by. Exactly 1.0 there.
    pub fn air_density_ratio(&self, altitude_m: f32) -> f32 {
        let reference = Self::DEFAULT.air_density(0.0);
        let here = self.air_density(altitude_m);
        if here == reference {
            1.0
        } else {
            here / reference
        }
    }

    /// Bakes the weather's grip into a session's own copy of the track:
    /// every centerline sample's grip, the surface's base figure (which the
    /// racing line and the AI's speed profile read) and the curb and
    /// off-track grips. Physics, the AI and the racing line then follow
    /// without a branch in the hot loop, and a dry session is the track to
    /// the bit.
    pub fn apply_to_track(&self, track: &mut TrackConfig) {
        let latitude = track.latitude_deg();
        track.track_surface.air_temperature_c = self.air_temperature_c();
        track.track_surface.track_temperature_c = self.track_temperature_c_at(latitude);
        track.track_surface.wet = self.weather.is_wet() || self.weather.is_snow();
        track.track_surface.water = self.weather.water();
        track.track_surface.baked_water = self.weather.water();
        track.track_surface.baked_snow = self.weather.snowfall();
        let altitude = track.metadata.altitude_m.unwrap_or(0.0);
        track.track_surface.air_density_ratio = self.air_density_ratio(altitude);
        track.track_surface.wind_mps = wind_onto_track(track, self.wind_mps());
        track.track_surface.wind_now_mps = track.track_surface.wind_mps;
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
        self.sun_elevation_at(DEFAULT_LATITUDE_DEG, self.time_of_day_minutes as f32)
    }

    /// The air temperature, °C, for this weather at this hour, on the sky
    /// model's late-May day at 50° N: 13 °C before dawn to 23 °C at 15:00
    /// in the sun, the swing damped under cloud and a few degrees lower in
    /// the rain. Until a host can pick one, this is the session's air.
    pub fn air_temperature_c(&self) -> f32 {
        // Whole degrees either way, as a session names them.
        match self.air_temp_c {
            Some(t) => t as f32,
            None => self.auto_air_temperature_c().round(),
        }
    }

    /// The weather's and the clock's own air temperature, °C.
    pub fn auto_air_temperature_c(&self) -> f32 {
        const COOLEST_C: f32 = 13.0;
        const SWING_C: f32 = 10.0;
        let warmest = self.solar_noon_hours() + WARMEST_AFTER_NOON_H;
        let hours = (self.time_of_day_minutes % Self::MINUTES_PER_DAY) as f32 / 60.0;
        let day = 0.5 + 0.5 * ((hours - warmest) / 24.0 * std::f32::consts::TAU).cos();
        let (swing, offset) = match self.weather {
            Weather::Sunny => (1.0, 0.0),
            Weather::Cloudy => (0.8, -0.5),
            Weather::Overcast => (0.5, -1.5),
            Weather::LightRain => (0.4, -3.0),
            Weather::HeavyRain => (0.3, -4.0),
            // Overcast's own day, 17 degrees colder: -4 to 1 °C.
            Weather::Snow => (0.4, -2.5 - Weather::SNOW_AIR_DROP_C),
        };
        COOLEST_C + SWING_C * (0.5 + (day - 0.5) * swing) + offset
    }

    /// The weather's and the clock's own air temperature, °C, on the date
    /// at a site: the reference day's ([`Self::auto_air_temperature_c`])
    /// moved by [`Self::climate_offset_c`]. Snow's own air is the same near
    /// freezing in any month and place: what the climate moves is how far
    /// the sky without it stands above it ([`Self::snow_air_drop_c`]).
    pub fn auto_air_temperature_c_at(&self, site: impl Into<ClimateSite>) -> f32 {
        let own = self.auto_air_temperature_c();
        if self.weather.is_snow() {
            own
        } else {
            own + self.climate_offset_c(site)
        }
    }

    /// How far snow's own air lies under overcast's on the date at a site,
    /// °C: [`Weather::SNOW_AIR_DROP_C`] on the reference day, less in a
    /// winter month or up a mountain (the overcast sky is nearer freezing,
    /// so a winter sky is a smaller step away), more in a summer one or in
    /// the tropics. The snow line.
    pub fn snow_air_drop_c(&self, site: impl Into<ClimateSite>) -> f32 {
        (Weather::SNOW_AIR_DROP_C + self.climate_offset_c(site)).max(0.0)
    }

    /// The asphalt, °C: the air plus what the sun puts into it through the
    /// cloud (up to 20 °C more under a high sun on a clear day). A wet
    /// track sits a degree under the air.
    pub fn track_temperature_c(&self) -> f32 {
        self.track_temperature_c_at(DEFAULT_LATITUDE_DEG)
    }

    /// The asphalt, °C, under the sun of a circuit `latitude_deg` north.
    pub fn track_temperature_c_at(&self, latitude_deg: f32) -> f32 {
        let air = self.air_temperature_c();
        if self.weather.is_wet() || self.weather.is_snow() {
            return air - 1.0;
        }
        let sun = self.sun_elevation_at(latitude_deg, self.time_of_day_minutes as f32);
        air + solar_gain_c(sun, self.weather.cloud())
    }

    /// Whether a car's lights are on when nobody has touched the switch:
    /// as the sun goes (under 6°) and in the rain, the client's
    /// `FSkyState::bHeadlights`.
    pub fn headlights_needed(&self) -> bool {
        self.sun_elevation_deg() < 6.0 || self.weather.is_wet() || self.weather.is_snow()
    }

    pub fn is_night(&self) -> bool {
        let hour = self.time_of_day_minutes / 60;
        !(6..20).contains(&hour)
    }
}

/// A wind in the start straight's frame (x down the straight, y to its
/// left) turned into the track's: the circuits keep their real
/// orientation, so the line's heading is not +X on every track.
pub fn wind_onto_track(track: &TrackConfig, [wx, wy]: [f32; 2]) -> [f32; 2] {
    let heading = track.centerline.first().map_or(0.0, |p| p.heading_rad);
    let (c, sn) = (heading.cos(), heading.sin());
    [wx * c - wy * sn, wx * sn + wy * c]
}

/// How long after solar noon the day's air is warmest, hours: 15:00 where
/// noon is 13:00.
pub const WARMEST_AFTER_NOON_H: f32 = 2.0;

/// The day of the year the air is warmest north of the equator (19 July;
/// six months on south of it), about a month after midsummer.
pub const WARMEST_DAY_OF_YEAR: f32 = 200.0;
/// The air's annual range (warmest month's mean less coldest's) per degree
/// of latitude, °C, up to [`SEASON_RANGE_MAX_C`]: none on the equator,
/// 17.5 °C at 50° (central Europe), about 9 °C at 25° (the Gulf, Florida).
pub const SEASON_RANGE_PER_DEG_C: f32 = 0.35;
pub const SEASON_RANGE_MAX_C: f32 = 20.0;
/// A degree of seasonal cold makes the weather's own air this much damper,
/// percent of relative humidity ([`SessionConditions::auto_humidity_pct_at`]).
pub const SEASON_HUMIDITY_PER_C: f32 = 1.0;
/// The driest the season makes the weather's own air, percent.
pub const SEASON_HUMIDITY_MIN_PCT: f32 = 20.0;

/// The season's air on `day` over a circuit `latitude_deg` north (south
/// negative) against the year's mean, °C: a cosine over the year, warmest
/// on [`WARMEST_DAY_OF_YEAR`] in the north and half a year on in the
/// south, swinging half the latitude's annual range either way.
pub fn season_air_c(day: u16, latitude_deg: f32) -> f32 {
    let range = (SEASON_RANGE_PER_DEG_C * latitude_deg.abs().min(90.0)).min(SEASON_RANGE_MAX_C);
    let warmest = if latitude_deg < 0.0 {
        WARMEST_DAY_OF_YEAR + 0.5 * DAYS_PER_YEAR as f32
    } else {
        WARMEST_DAY_OF_YEAR
    };
    let phase = (day as f32 - warmest) / DAYS_PER_YEAR as f32 * std::f32::consts::TAU;
    0.5 * range * phase.cos()
}

/// The latitude the sky is worked out for when a circuit does not say
/// where it is, degrees north: the client sky model's (`ApexSky`).
pub const DEFAULT_LATITUDE_DEG: f32 = 50.0;
/// The model day's solar declination, degrees: a day in late May.
pub const SUN_DECLINATION_DEG: f32 = 20.0;
/// The model day, day of the year: 25 May.
pub const MODEL_DAY_OF_YEAR: u16 = 145;
pub const DAYS_PER_YEAR: u16 = 365;
/// Solar noon on the local clock (summer time) where the track does not
/// say where it is.
pub const SOLAR_NOON_HOURS: f32 = 13.0;
/// What a high sun through a clear sky puts into the asphalt over the air.
pub const SOLAR_GAIN_C: f32 = 20.0;

/// The sun's elevation, degrees, `minutes` after midnight at `latitude_deg`
/// north (south negative), the sun at its height at `noon_hours` on a day
/// of `declination_deg`: the client's `ApexSky::SunAt` to the letter.
pub fn sun_elevation_deg(
    latitude_deg: f32,
    minutes: f32,
    noon_hours: f32,
    declination_deg: f32,
) -> f32 {
    let hours = minutes.rem_euclid(SessionConditions::MINUTES_PER_DAY as f32) / 60.0;
    let lat = latitude_deg.clamp(-89.0, 89.0).to_radians();
    let dec = declination_deg.to_radians();
    let hour_angle = ((hours - noon_hours) * 15.0).to_radians();
    let sin_elev = lat.sin() * dec.sin() + lat.cos() * dec.cos() * hour_angle.cos();
    sin_elev.clamp(-1.0, 1.0).asin().to_degrees()
}

/// The sun's declination on day `day` of the year, degrees (Cooper's
/// formula): +23.4 at midsummer in the north, -23.4 at midwinter. The
/// client's `ApexSky::DeclinationOn`.
pub fn sun_declination_deg(day: u16) -> f32 {
    let n = day.clamp(1, DAYS_PER_YEAR) as f32;
    23.44 * ((284.0 + n) / DAYS_PER_YEAR as f32 * std::f32::consts::TAU).sin()
}

/// The equation of time on day `day`, minutes: how far the sun runs ahead
/// of the mean clock (+16 in early November, -14 in mid February).
pub fn equation_of_time_min(day: u16) -> f32 {
    let b = (day.clamp(1, DAYS_PER_YEAR) as f32 - 81.0) / 364.0 * std::f32::consts::TAU;
    9.87 * (2.0 * b).sin() - 7.53 * b.cos() - 1.5 * b.sin()
}

/// Summer time: which part of the year, if any, a circuit's clocks go on
/// an hour.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum SummerTime {
    None,
    /// The European rule: the end of March to the end of October.
    Europe,
    /// The North American rule: the middle of March to the start of
    /// November.
    America,
    /// The southern rule: the start of October to the start of April.
    South,
}

impl SummerTime {
    fn hours_on(self, day: u16) -> f32 {
        let on = match self {
            SummerTime::None => false,
            SummerTime::Europe => (88..300).contains(&day),
            SummerTime::America => (69..307).contains(&day),
            SummerTime::South => !(95..278).contains(&day),
        };
        if on {
            1.0
        } else {
            0.0
        }
    }
}

/// The legal clock over a circuit at `latitude_deg`, `longitude_deg`: hours
/// over UTC in standard time and its summer time. A coarse map of where
/// racing happens (most of Europe keeps Central European time well west of
/// its meridian, Spain and France included; the Americas' zones are cut by
/// longitude); elsewhere the meridian's own hour with no summer time, which
/// is right for the Gulf, Asia, Japan and Brazil.
fn legal_clock(latitude_deg: f32, longitude_deg: f32) -> (f32, SummerTime) {
    let (lat, lon) = (latitude_deg, longitude_deg);
    let within = |lat_lo: f32, lat_hi: f32, lon_lo: f32, lon_hi: f32| {
        (lat_lo..lat_hi).contains(&lat) && (lon_lo..lon_hi).contains(&lon)
    };
    // Britain and Ireland, then Portugal: Greenwich time.
    if within(49.8, 61.0, -11.0, 1.8) || within(36.8, 42.2, -9.6, -6.2) {
        return (0.0, SummerTime::Europe);
    }
    // Spain to Poland, Italy to Norway: Central European time.
    if within(35.0, 71.0, -9.6, 24.0) {
        return (1.0, SummerTime::Europe);
    }
    // Finland, the Baltics, Romania, Greece: Eastern European time.
    if within(34.5, 71.0, 24.0, 30.0) {
        return (2.0, SummerTime::Europe);
    }
    // Singapore and Malaysia keep China's hour.
    if within(-2.0, 8.0, 99.0, 120.0) {
        return (8.0, SummerTime::None);
    }
    // Most of Mexico: Central time, no summer time since 2022.
    if within(14.0, 25.8, -118.0, -86.0) {
        return (-6.0, SummerTime::None);
    }
    // Argentina and Uruguay.
    if within(-56.0, -21.0, -74.0, -53.0) {
        return (-3.0, SummerTime::None);
    }
    // The United States and Canada, by zone.
    if within(25.0, 72.0, -170.0, -50.0) {
        let hours = if lon >= -67.0 && lat >= 43.0 {
            -4.0
        } else if lon >= -87.5 {
            -5.0
        } else if lon >= -102.0 {
            -6.0
        } else if lon >= -114.0 {
            -7.0
        } else {
            -8.0
        };
        return (hours, SummerTime::America);
    }
    // Australia: the south-east keeps summer time, Queensland and the west
    // do not.
    if within(-44.0, -10.0, 113.0, 154.0) {
        let hours = if lon >= 141.0 {
            10.0
        } else if lon >= 129.0 {
            9.5
        } else {
            8.0
        };
        let summer = if lat < -28.2 && lon >= 129.0 {
            SummerTime::South
        } else {
            SummerTime::None
        };
        return (hours, summer);
    }
    ((lon / 15.0).round(), SummerTime::None)
}

/// Solar noon on a circuit's legal clock, minutes after midnight, on day
/// `day` of the year: 12:00 moved by the circuit's distance from its
/// zone's meridian (four minutes a degree), summer time and the equation
/// of time.
pub fn solar_noon_minutes(latitude_deg: f32, longitude_deg: f32, day: u16) -> u16 {
    let (zone_h, summer) = legal_clock(latitude_deg, longitude_deg);
    let hours = 12.0 + zone_h + summer.hours_on(day)
        - longitude_deg / 15.0
        - equation_of_time_min(day) / 60.0;
    (hours * 60.0)
        .round()
        .rem_euclid(SessionConditions::MINUTES_PER_DAY as f32) as u16
}

/// What the sun at `sun_elevation_deg` puts into dry asphalt through a sky
/// `cloud` covered (0 clear, 1 overcast), °C over the air.
pub fn solar_gain_c(sun_elevation_deg: f32, cloud: f32) -> f32 {
    let sun = sun_elevation_deg.to_radians().sin().max(0.0);
    SOLAR_GAIN_C * sun * through_cloud(cloud)
}

/// The share of the sun's heat that gets through a sky `cloud` covered:
/// the weather's own figures (clear 1, cloudy 0.6, overcast and rain
/// 0.25), linear between.
pub fn through_cloud(cloud: f32) -> f32 {
    let c = cloud.clamp(0.0, 1.0);
    let half = Weather::Cloudy.cloud();
    let full = Weather::Overcast.cloud();
    if c <= half {
        1.0 - 0.4 * c / half
    } else if c <= full {
        0.6 - 0.35 * (c - half) / (full - half)
    } else {
        0.25
    }
}

/// Density of moist air, kg/m³, at `altitude_m` (the standard
/// atmosphere's pressure), `temperature_c` and relative `humidity` (0..1).
pub fn air_density(altitude_m: f32, temperature_c: f32, humidity: f32) -> f32 {
    let pressure =
        101_325.0 * (1.0 - 2.255_77e-5 * altitude_m.clamp(-500.0, 5000.0)).powf(5.255_88);
    let kelvin = temperature_c + 273.15;
    // Tetens: the vapour's saturation pressure, Pa.
    let saturation = 610.78 * 10f32.powf(7.5 * temperature_c / (temperature_c + 237.3));
    let vapour = humidity.clamp(0.0, 1.0) * saturation;
    (pressure - vapour) / (287.058 * kelvin) + vapour / (461.495 * kelvin)
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
    /// Qualification mode: practice with timing; the session's best legal laps
    /// classify the drivers (`GameSession::qualifying_order`), no fixed length yet
    Qualification = 6,
    /// Race mode
    Race = 7,
    /// Time attack: every driver starts in the garage (tuning, nothing
    /// simulated), goes out onto a run-up before the line for flying laps,
    /// and can come back in at any time (`ClientMessage::HotlapRelocate`).
    /// Cars in the garage are neither ticked nor collided with, so several
    /// drivers can hotlap side by side on one track.
    Hotlap = 8,
    /// Timed practice, a race weekend's first phase: run like qualifying
    /// (garages, an outlap from the pit exit on cold tyres, timed laps and
    /// sectors, a clock and a chequered flag), but nothing classifies for
    /// the grid or goes to the stored qualifying results.
    Practice = 9,
    /// The race's grid before its countdown: the cars stand on their slots
    /// as in `Countdown`, with no clock, while each human picks setup, tyres
    /// and livery and says they are ready (`ClientMessage::SetReady`). When
    /// every connected human is, the countdown into the race starts on its
    /// own; the host's `StartCountdown` starts it at once.
    Grid = 10,
}

impl GameMode {
    /// Every human starts in a garage and goes out with `HotlapRelocate`:
    /// hotlap, qualifying and timed practice (`GameSession::tick_hotlap`).
    pub fn uses_garages(self) -> bool {
        matches!(
            self,
            GameMode::Hotlap | GameMode::Qualification | GameMode::Practice
        )
    }

    /// Going out is an outlap from the pit exit on cold tyres, and a length
    /// set for the session puts a clock and a chequered flag on it:
    /// qualifying and timed practice.
    pub fn has_outlap(self) -> bool {
        matches!(self, GameMode::Qualification | GameMode::Practice)
    }
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

/// The shortest timed race a host can set (`CreateSession.race_seconds`).
pub const MIN_RACE_SECONDS: u32 = 60;
/// The longest: twenty-four hours.
pub const MAX_RACE_SECONDS: u32 = 24 * 3600;

/// A timed race's length as a session keeps it: `None` (a race over laps)
/// for none or zero, otherwise held between [`MIN_RACE_SECONDS`] and
/// [`MAX_RACE_SECONDS`].
pub fn clamp_race_seconds(seconds: Option<u32>) -> Option<u32> {
    seconds
        .filter(|s| *s > 0)
        .map(|s| s.clamp(MIN_RACE_SECONDS, MAX_RACE_SECONDS))
}

/// The shortest timed qualifying session a host can set
/// (`CreateSession.qualifying_seconds`).
pub const MIN_QUALIFYING_SECONDS: u32 = 60;
/// The longest: two hours.
pub const MAX_QUALIFYING_SECONDS: u32 = 2 * 3600;

/// A timed qualifying session's length as a session keeps it: `None`
/// (open-ended qualifying) for none or zero, otherwise held between
/// [`MIN_QUALIFYING_SECONDS`] and [`MAX_QUALIFYING_SECONDS`].
pub fn clamp_qualifying_seconds(seconds: Option<u32>) -> Option<u32> {
    seconds
        .filter(|s| *s > 0)
        .map(|s| s.clamp(MIN_QUALIFYING_SECONDS, MAX_QUALIFYING_SECONDS))
}

/// The tightest qualifying cut-off a host can set
/// (`CreateSession.qualifying_cutoff_pct`), percent of pole: 101%.
pub const MIN_QUALIFYING_CUTOFF_PCT: u16 = 101;
/// The loosest: 150%.
pub const MAX_QUALIFYING_CUTOFF_PCT: u16 = 150;

/// The qualifying cut-off (the 107% rule) as a session keeps it: `None`
/// (no cut-off) for none or zero, otherwise held between
/// [`MIN_QUALIFYING_CUTOFF_PCT`] and [`MAX_QUALIFYING_CUTOFF_PCT`].
pub fn clamp_qualifying_cutoff(pct: Option<u16>) -> Option<u16> {
    pct.filter(|p| *p > 0)
        .map(|p| p.clamp(MIN_QUALIFYING_CUTOFF_PCT, MAX_QUALIFYING_CUTOFF_PCT))
}

/// How qualifying is run (`CreateSession.qualifying_format`): the usual
/// session with every car out at once, or a one-lap shoot-out. Encoded as a
/// small integer; `Open` (the default) is left off the wire.
#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize_repr, Deserialize_repr, Default)]
pub enum QualifyingFormat {
    /// Every car out when it likes, timed or open-ended
    /// (`qualifying_seconds`); a weekend's rounds are a timed knockout.
    #[default]
    Open = 0,
    /// One car at a time in a fixed order, each an outlap from the pit exit
    /// and one flying lap, the rest in their garages; the classification is
    /// the flying laps (`GameSession::begin_shootout`). A weekend runs its
    /// final round this way.
    Shootout = 1,
}

impl QualifyingFormat {
    pub fn is_open(&self) -> bool {
        *self == QualifyingFormat::Open
    }
}

/// The most flying laps a qualifying lap allowance can give
/// (`CreateSession.qualifying_lap_allowance`).
pub const MAX_QUALIFYING_LAP_ALLOWANCE: u8 = 30;

/// A qualifying lap allowance as a session keeps it: 0 for none, otherwise
/// held to [`MAX_QUALIFYING_LAP_ALLOWANCE`].
pub fn clamp_qualifying_lap_allowance(laps: u8) -> u8 {
    laps.min(MAX_QUALIFYING_LAP_ALLOWANCE)
}

/// The most mandatory stops a host can set (`CreateSession.mandatory_stops`).
pub const MAX_MANDATORY_STOPS: u8 = 3;

/// The race's pit-stop rule (`CreateSession.mandatory_stops` and
/// `compound_change`): the stops every car must make in a race, and whether
/// it must race on two dry compounds. A car that takes the flag owing
/// either is classified behind every finisher that met it
/// (`GameSession::assign_finish_positions`, `RosterEntry::missed_stops`).
/// The default is no rule.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct PitRule {
    /// Stops (a service in the car's box) owed in the race, at most
    /// [`MAX_MANDATORY_STOPS`].
    #[serde(default)]
    pub mandatory_stops: u8,
    /// The car must run a second dry compound in the race: met by a stop
    /// that fits a different compound, and waived for a car that runs an
    /// intermediate or wet tyre at any point of the race.
    #[serde(default)]
    pub compound_change: bool,
}

impl PitRule {
    /// The rule as a session keeps it, the stops held to
    /// [`MAX_MANDATORY_STOPS`].
    pub fn clamped(mandatory_stops: u8, compound_change: bool) -> Self {
        Self {
            mandatory_stops: mandatory_stops.min(MAX_MANDATORY_STOPS),
            compound_change,
        }
    }

    pub fn is_none(&self) -> bool {
        self.mandatory_stops == 0 && !self.compound_change
    }

    /// The stops a car that has made `stops` still owes, and whether it
    /// still owes the second compound (`changed`: it has run two dry
    /// compounds or a treaded tyre).
    pub fn owed(&self, stops: u16, changed: bool) -> (u32, bool) {
        let stops_owed = (self.mandatory_stops as u32).saturating_sub(stops as u32);
        (stops_owed, self.compound_change && !changed)
    }
}

/// The most classes a multi-class field may have
/// (`CreateSession.classes`).
pub const MAX_CLASSES: usize = 4;

/// One class of a multi-class field (`CreateSession.classes`,
/// `RaceSession::classes`): the cars of a car.toml `class`, how many AI
/// drive them, and the level those AI are seated at. A session with two
/// or more deals its AI per class, grids the field by class, classifies
/// each car in its class as well as overall, shows a car the blue flag
/// for a faster class closing on it, and has the AI yield across classes
/// (`GameSession::class_rank_of`). The server keeps the list in rank
/// order, the quickest class first, by each class's planned lap on the
/// session's track (`GameSession::rank_classes`).
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct ClassEntry {
    /// The car.toml `class` (`GT3`, `LMP2`, ...), matched ignoring case.
    pub class: String,
    /// AI cars of the class.
    #[serde(default)]
    pub ai_count: u8,
    /// The level the class's AI are seated at
    /// (`crate::ai_driver::field_skills`); the session's `ai_skill` when
    /// absent, and left off the wire then.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub ai_skill: Option<u8>,
}

impl ClassEntry {
    /// The list as a session keeps it: names trimmed, blank ones and
    /// repeats dropped (the first of a name wins, ignoring case), at most
    /// [`MAX_CLASSES`], each level clamped.
    pub fn sanitize(classes: Vec<ClassEntry>) -> Vec<ClassEntry> {
        let mut out: Vec<ClassEntry> = Vec::new();
        for mut entry in classes {
            entry.class = entry.class.trim().to_string();
            if entry.class.is_empty()
                || out
                    .iter()
                    .any(|c| c.class.eq_ignore_ascii_case(&entry.class))
            {
                continue;
            }
            entry.ai_skill = entry.ai_skill.map(crate::ai_driver::clamp_skill);
            out.push(entry);
            if out.len() >= MAX_CLASSES {
                break;
            }
        }
        out
    }

    /// Whether `class` (a car's) is this class.
    pub fn is(&self, class: &str) -> bool {
        self.class.eq_ignore_ascii_case(class.trim())
    }
}

/// How a race is started (`RaceRules::start`). Encoded as a small integer;
/// `Standing` (the default) is left off the wire.
#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize_repr, Deserialize_repr, Default)]
pub enum StartType {
    /// From the grid, at rest, when the lights go out.
    #[default]
    Standing = 0,
    /// Behind a pace lap, the field running in grid order: green as the
    /// leader comes back to the line (`crate::race_start`).
    Rolling = 1,
}

impl StartType {
    pub fn is_standing(&self) -> bool {
        *self == StartType::Standing
    }
}

/// What neutralises a race when a car is stranded on the road
/// (`RaceRules::safety_car`). Encoded as a small integer; `Off` (the
/// default) is left off the wire.
#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize_repr, Deserialize_repr, Default)]
pub enum SafetyCar {
    /// Yellow flags only.
    #[default]
    Off = 0,
    /// A virtual safety car (full-course yellow): every car held to a
    /// pace, no overtaking, the pit lane open (`crate::race_control`).
    Virtual = 1,
    /// A safety car on the road (`crate::safety_car`): the field queues
    /// behind it, lapped cars are let by, the pit lane is closed while the
    /// queue forms, and the leader restarts the race at the line.
    Full = 2,
}

impl SafetyCar {
    pub fn is_off(&self) -> bool {
        *self == SafetyCar::Off
    }
}

/// The race-control rules a host sets on create (`CreateSession.race_rules`,
/// echoed in `SessionJoined.RaceRules`): how the race starts and what is
/// penalised. Every field at its default is a race as before, and is left
/// off the wire; so is the whole table when nothing is set.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct RaceRules {
    /// Standing or rolling start ([`StartType`]).
    #[serde(default, skip_serializing_if = "StartType::is_standing")]
    pub start: StartType,
    /// A standing start is preceded by a formation lap: the field drives a
    /// lap behind the leader at a capped pace, no overtaking, and lines up
    /// again on the grid (`crate::race_start`). A rolling start always has
    /// its pace lap.
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub formation_lap: bool,
    /// False starts: through the countdown a driver's car is not held on
    /// its slot, and one that moves off it before the lights go out has
    /// jumped the start (`crate::race_start::JUMP_START_M`), penalised with
    /// a drive-through.
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub jump_start: bool,
    /// A car that passes another of its lap in a yellow sector is given a
    /// time penalty (`race_control::YELLOW_PASS_PENALTY_S`), unless the car
    /// passed is the incident or slowed by it.
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub yellow_penalty: bool,
    /// A driver who drives the pit lane themselves and leaves it past its
    /// red exit light is given a stop-go
    /// (`race_control::PIT_EXIT_STOP_GO_S`).
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub pit_exit_penalty: bool,
    /// A car stranded on the road brings out a virtual safety car or a
    /// safety car on the road ([`SafetyCar`]).
    #[serde(default, skip_serializing_if = "SafetyCar::is_off")]
    pub safety_car: SafetyCar,
    /// A reverse grid: the first this many slots of the starting order
    /// (the host's order, the qualifying result or the weekend's grid) start
    /// in reverse, so P1 starts from slot N. 0 and 1 change nothing
    /// (`GameSession::grid_order_ids`).
    #[serde(default, skip_serializing_if = "is_zero_u8")]
    pub reverse_grid: u8,
    /// The qualifying tyre allowance: the new slick sets a car may take in
    /// qualifying (every round of a weekend's together). Past it a car goes
    /// out on its least worn used set (`crate::tyre_sets`). 0 is none.
    #[serde(default, skip_serializing_if = "is_zero_u8")]
    pub qualifying_tyre_sets: u8,
    /// The first this many cars of the qualifying classification start the
    /// race on the set they set their placing lap on, worn as it was (255:
    /// every car with a time). Waived when the race starts wet. 0 is off.
    #[serde(default, skip_serializing_if = "is_zero_u8")]
    pub qualifying_tyre_start: u8,
    /// Parc fermé: from a car's first run in qualifying to the end of the
    /// race its setup is held, all but the knobs a driver may still touch
    /// (`CarSetup::under_parc_ferme`).
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub parc_ferme: bool,
    /// A weekend's tyre allocation: the new slick sets a car may take in its
    /// practice and qualifying together; the sets carry from one phase to
    /// the next, and past it a car goes out on its least worn used set.
    /// The race's stops fit new sets. 0 is none.
    #[serde(default, skip_serializing_if = "is_zero_u8")]
    pub weekend_tyre_sets: u8,
}

fn is_zero_u8(v: &u8) -> bool {
    *v == 0
}

impl RaceRules {
    pub fn is_default(&self) -> bool {
        *self == RaceRules::default()
    }

    /// Whether `offence` is penalised under these rules.
    pub fn penalises(&self, offence: crate::race_control::Offence) -> bool {
        use crate::race_control::Offence;
        match offence {
            Offence::JumpStart => self.jump_start,
            Offence::YellowPass => self.yellow_penalty,
            Offence::PitExitRed => self.pit_exit_penalty,
        }
    }

    /// The start drives a lap before the green: a formation lap, or a
    /// rolling start's pace lap.
    pub fn has_formation_lap(&self) -> bool {
        self.formation_lap || self.start == StartType::Rolling
    }
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
    /// How much damage every car here takes; see `DamageLevel`.
    #[serde(default)]
    pub damage: DamageLevel,
    /// The level the host picked for the AI field
    /// (`crate::ai_driver::field_skills`); `None` is the mixed field.
    #[serde(default)]
    pub ai_skill: Option<u8>,
    /// The race's start order the host asked for, as driver references in
    /// grid order (see `GameSession::apply_grid_order`); empty means the
    /// order cars were seated in, or the session's own qualifying result.
    #[serde(default)]
    pub grid_order: Vec<String>,
    pub state: SessionState,
    #[serde(default)]
    pub game_mode: GameMode,
    /// Keyed and iterated in PlayerId order (BTreeMap): physics and the
    /// order-dependent collision solver must be deterministic across runs.
    pub participants: std::collections::BTreeMap<PlayerId, CarState>,
    pub max_players: u8,
    pub ai_count: u8,
    /// A multi-class field's classes ([`ClassEntry`]), in rank order, the
    /// quickest first; empty for a field of the host car's class alone.
    #[serde(default)]
    pub classes: Vec<ClassEntry>,
    /// The race distance in laps; 0 in a timed race (see `race_seconds`).
    pub lap_limit: u8,
    /// A timed race's length, from the green light
    /// (`clamp_race_seconds`); `None` for a race over `lap_limit` laps.
    /// When it runs out the leader's lap is the last
    /// (`GameSession::race_clock`).
    #[serde(default)]
    pub race_seconds: Option<u32>,
    /// A timed qualifying session's length, from entering
    /// `GameMode::Qualification` (`clamp_qualifying_seconds`); `None` for
    /// open-ended qualifying, which never finishes. When it runs out the
    /// chequered flag is out (`GameSession::race_clock`).
    #[serde(default)]
    pub qualifying_seconds: Option<u32>,
    /// A race weekend's format (`crate::weekend`), clamped; `None` for a
    /// plain session. A weekend's rounds carry their own lengths, so
    /// `qualifying_seconds` is `None` beside it.
    #[serde(default)]
    pub weekend: Option<crate::weekend::WeekendFormat>,
    /// The qualifying cut-off, percent of pole (`clamp_qualifying_cutoff`;
    /// 107 is the 107% rule): a car slower than this in qualifying, or with
    /// no time, does not start the race (`GameSession::non_qualifiers`).
    /// `None` for no cut-off.
    #[serde(default)]
    pub qualifying_cutoff_pct: Option<u16>,
    /// The race's pit-stop rule ([`PitRule`]); none by default.
    #[serde(default)]
    pub pit_rule: PitRule,
    /// How qualifying runs ([`QualifyingFormat`]): a weekend runs its final
    /// round as a shoot-out under `Shootout`.
    #[serde(default)]
    pub qualifying_format: QualifyingFormat,
    /// The most flying laps a car may start in a qualifying session or
    /// round (`clamp_qualifying_lap_allowance`); 0 for no limit. A
    /// shoot-out gives one whatever it says.
    #[serde(default)]
    pub qualifying_lap_allowance: u8,
    /// The race-control rules ([`RaceRules`]): the start, false starts,
    /// penalties. All off by default.
    #[serde(default)]
    pub race_rules: RaceRules,
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
            damage: DamageLevel::Full,
            ai_skill: None,
            grid_order: Vec::new(),
            state: SessionState::Lobby,
            game_mode: GameMode::Lobby,
            participants: std::collections::BTreeMap::new(),
            max_players,
            ai_count,
            classes: Vec::new(),
            lap_limit,
            race_seconds: None,
            qualifying_seconds: None,
            qualifying_cutoff_pct: None,
            pit_rule: PitRule::default(),
            qualifying_format: QualifyingFormat::Open,
            qualifying_lap_allowance: 0,
            race_rules: RaceRules::default(),
            weekend: None,
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
    /// The hybrid's mode (`crate::hybrid::ErsMode`); `None` keeps the
    /// car's.
    pub ers_mode: Option<u8>,
    /// The overtake button is held: full deployment whatever the mode.
    pub ers_boost: bool,
    /// The handbrake is held: the rear wheels are braked hard and ABS is
    /// off for them (`physics::HANDBRAKE_SHARE`).
    pub handbrake: bool,
}

impl PlayerInputData {
    /// The input as the server lets it reach the physics: `None` with a
    /// non-finite axis, else every axis clamped into its range and a gear
    /// outside reverse..10th dropped.
    pub fn sanitized(self) -> Option<Self> {
        if !self.throttle.is_finite()
            || !self.brake.is_finite()
            || !self.steering.is_finite()
            || self.clutch.is_some_and(|c| !c.is_finite())
        {
            return None;
        }
        Some(Self {
            throttle: self.throttle.clamp(0.0, 1.0),
            brake: self.brake.clamp(0.0, 1.0),
            steering: self.steering.clamp(-1.0, 1.0),
            gear: self.gear.filter(|g| (-1..=10).contains(g)),
            clutch: self.clutch.map(|c| c.clamp(0.0, 1.0)),
            ..self
        })
    }

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
            ers_mode: None,
            ers_boost: false,
            handbrake: false,
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
        assert!(!Weather::Snow.is_wet() && Weather::Snow.is_snow());
        assert_eq!(Weather::from_u8(5), Some(Weather::Snow));
        assert_eq!(Weather::from_u8(6), None);
        assert!((Weather::Snow.road_grip_factor() - 0.28).abs() < 1e-6);
        assert_eq!(Weather::Snow.rainfall(), 0.0);
        assert_eq!(Weather::Snow.snowfall(), 1.0);
        assert_eq!(Weather::HeavyRain.snowfall(), 0.0);
    }

    #[test]
    fn conditions_clamp_wraps_the_clock_and_defaults_to_a_sunny_afternoon() {
        assert_eq!(SessionConditions::default(), SessionConditions::DEFAULT);
        assert_eq!(SessionConditions::DEFAULT.time_of_day_minutes, 13 * 60);
        let wrapped = SessionConditions {
            weather: Weather::Overcast,
            time_of_day_minutes: 24 * 60 + 5,
            ..SessionConditions::DEFAULT
        }
        .clamp();
        assert_eq!(wrapped.time_of_day_minutes, 5);
        assert_eq!(wrapped.weather, Weather::Overcast);
        assert!(wrapped.is_night());
        assert!(!SessionConditions::DEFAULT.is_night());
        assert_eq!(Weather::from_u8(4), Some(Weather::HeavyRain));
        assert_eq!(Weather::from_u8(5), Some(Weather::Snow));
        assert_eq!(Weather::from_u8(6), None);
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
            ..SessionConditions::DEFAULT
        })
        .unwrap();
        let decoded: SessionConditions = rmp_serde::from_slice(&full).unwrap();
        assert_eq!(decoded.weather, Weather::LightRain);
        assert_eq!(decoded.time_of_day_minutes, 1290);
    }
}
