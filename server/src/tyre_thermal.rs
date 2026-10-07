//! Tyre temperature: two thermal masses per tyre, the tread in three
//! zones across its width, and what their heat does to the grip and the
//! pressure; the compounds, the wear, the flat spots and the punctures.
//!
//! Each tyre is a **tread** (the rubber on the road: light, heated by the
//! power the contact patch dissipates sliding, cooled by the air and by the
//! road it rolls onto) and a **core** (carcass, gas and some of the wheel:
//! heavy, heated by the tyre flexing as it rolls and by the tread above it,
//! cooled by the air inside the wheel arch). The tread answers a slide in a
//! corner or two and cools down a straight; the core takes laps, and the
//! pressure follows it.
//!
//! The tread is **three zones** across its width ([`TireData::tread_c`]:
//! inner shoulder, middle, outer shoulder; `temperature_c` is their mean).
//! Where the patch's heat lands is where the patch is ([`zone_shares`]):
//! negative camber puts the inner shoulder on the road, a lateral force
//! toward the car rolls the carcass onto the outer shoulder, and an
//! over-inflated tyre crowns onto its middle. Heat conducts between the
//! zones ([`ZONE_CONDUCTANCE`]) and each sheds to the air and the road on
//! its own, so a tyre run on one shoulder shows it. The grip reads the
//! mean, so a tyre heated evenly is bit-identical to the one-node model.
//!
//! - **Grip** reads a blend of the two ([`TREAD_GRIP_SHARE`]): full inside
//!   `optimal_temperature_c ± temperature_window_c`, falling off by
//!   `temperature_grip_falloff` per degree outside it, eased in over the
//!   first degrees so the edge of the window is not a step the car can
//!   feel. Cold is as bad as hot per degree, and much further away.
//! - **Pressure** is the gas law on the core: the setup's pressures are
//!   what the tyre runs at *at its optimum*, so a cold tyre is soft and an
//!   overheated one is over-inflated, and the pressure grip curve
//!   (`TireConfig::pressure_grip_factor`) charges for both. A tyre in its
//!   window at the set pressure grips exactly what it did before
//!   temperatures existed. A **puncture** takes the air out
//!   ([`puncture`]): at once, or as a slow leak
//!   ([`TireData::leak_kpa_per_s`]) that the pressure curve charges for
//!   more every lap until the tyre is flat ([`PUNCTURE_FLAT_KPA`]).
//!
//! Heat in, per tyre: the slip power, `|Fx| · slip speed along + |Fy| ·
//! slip speed across`, is what the tyre dissipates to make its force.
//! Up to the tyre's peak slip it is the rubber's hysteresis as the patch
//! is sheared and let go every revolution, and it heats the rubber right
//! through: [`TREAD_HYSTERESIS_SHARE`] of it the tread layer, by mass,
//! the rest the bulk (the core here). Past the peak it is the rubber
//! sliding over the road, and friction at an interface heats both sides
//! by their thermal effusivities: only [`FRICTION_HEAT_TO_TYRE`] of that
//! goes into the tread, the rest into a road that is fresh under every
//! pass. In the **dirty air** of a car ahead the tyre works harder for
//! the same force (less downforce, more slip for every corner), and the
//! turbulence over it stirs the patch: [`DIRTY_AIR_SLIDE_HEAT`] of the
//! gripping power more goes into the tread per unit of downforce lost.
//! [`CARCASS_HEAT_SHARE`] of `rolling_resistance · load · speed`
//! (the carcass flexing) heats the core too. Heat out: convection to the
//! air, growing with speed to the 0.8 as a turbulent boundary layer does,
//! and conduction into the road through the patch, growing with the root
//! of speed as fresh road keeps arriving under it; a wet road takes
//! several times as much.
//!
//! The first version put 85% of the sub-peak slip power into the tread.
//! That is the whole of the power into a kilogram and a half of rubber,
//! and a careful driver felt it as "every slight turn is 10 °C": an LMP2
//! at 2° of slip at 70 m/s dissipates 22 kW per front tyre, which is 9 °C
//! a second in that layer, and Eau Rouge took its fronts from 85 to 150 °C
//! while the AI, smooth and at the peak, saw 109 on the same lap. The
//! tyre keeps the same energy now; it lands in the bulk, and the surface
//! follows it over a lap instead of leading it through every corner.
//! The surface layer's constants are now checked against a real tyre's
//! surface swing as well as the AI's windows
//! (`the_surface_swings_like_a_real_tyre_over_a_lap`): a GT slick's
//! surface rises 15-35 °C through a corner and falls back most of the way
//! down the straight after it, while the bulk moves a few degrees.
//!
//! **Compounds** are the car's own ([`TireConfig::compounds`], car.toml
//! `[[tires.compound]]`): each a change to the car's tyre (grip, wear,
//! window, grip in water, `kind`), one of them the *reference* the car
//! was calibrated on (the setup's zero). A car.toml that lists none has
//! [`default_compounds`]: soft, medium, hard, intermediate and wet, the
//! medium the reference.
//!
//! Everything is a pure function of the tick's inputs (no clock, no RNG),
//! so the sim stays deterministic.

use serde::{Deserialize, Serialize};

use crate::data::{CarState, TireConfig, TireData, TrackSurface};

/// Where a compound grips best, °C, for a car that does not say.
pub const DEFAULT_OPTIMAL_C: f32 = 90.0;
/// Half-width of the flat top of the grip curve, °C.
pub const DEFAULT_WINDOW_C: f32 = 10.0;
/// Grip lost per degree outside the window.
pub const DEFAULT_GRIP_FALLOFF: f32 = 0.004;

/// The falloff reaches its full slope over about this many degrees past
/// the window's edge (`off² / (off + EASE)`).
const FALLOFF_EASE_C: f32 = 4.0;
/// However cold or hot, a tyre keeps this much of its grip from temperature
/// alone (the pressure can take more).
const MAX_THERMAL_GRIP_LOSS: f32 = 0.25;
/// A cold tyre is charged this share of the falloff per degree: it is also
/// under-inflated, and the pressure curve charges for that.
const COLD_FALLOFF_SHARE: f32 = 0.6;
/// How much of the temperature the grip reads is the tread's; the rest is
/// the core's, which is what keeps a surface flash from being a grip spike.
pub const TREAD_GRIP_SHARE: f32 = 0.7;

const ATMOSPHERE_KPA: f32 = 101.325;
const KELVIN: f32 = 273.15;

/// Heat capacities, J/K: a tread of about a kilogram and a half of rubber
/// on a carcass, gas and hub of ten times that.
const TREAD_HEAT_CAPACITY: f32 = 2_000.0;
const CORE_HEAT_CAPACITY: f32 = 7_000.0;
/// Conduction between tread and core, W/K (a millimetre or two of rubber
/// under the surface).
const TREAD_CORE_CONDUCTANCE: f32 = 350.0;
/// Conduction between neighbouring tread zones, W/K: a few centimetres of
/// rubber sideways, so a shoulder run hot takes seconds to warm its
/// neighbour.
pub const ZONE_CONDUCTANCE: f32 = 60.0;
/// Convection from the tread to the air: still air, and the forced part
/// per (m/s)^0.8, W/K.
const TREAD_AIR_STILL: f32 = 5.0;
const TREAD_AIR_FORCED: f32 = 1.2;
/// The core sheds heat to the air inside the wheel at this share of the
/// tread's rate.
const CORE_AIR_SHARE: f32 = 0.8;
/// Conduction into the road per root of speed (m/s), W/K, and how many
/// times that a wet road takes.
const TREAD_ROAD_CONDUCTANCE: f32 = 3.5;
const WET_ROAD_FACTOR: f32 = 4.0;
/// Share of the slip power below the peak slip (the rubber's hysteresis,
/// spread through it) that the tread layer takes, by mass: the surface
/// layer and the tread blocks under it, which flex the most. The bulk
/// takes the rest. Handing part of that power to the road instead was
/// tried (a quarter of it in the tyre: the LMP2's fronts ran at 66 °C at
/// Monza against a window of 82-102; 0.55: 72; 0.9: 78): the AI at its
/// peak slip sets every class's window, and the tyre has to keep the
/// energy it kept before. Only where it lands changed.
const TREAD_HYSTERESIS_SHARE: f32 = 0.3;
/// Share of the friction power at the patch (the rubber sliding over the
/// road: everything past the peak slip) that goes into the tyre at all.
/// Friction heats the interface, and
/// the heat splits between rubber and asphalt by their thermal
/// effusivities (about 0.7 and 1.5 kJ/m²K√s): roughly a third to the tyre,
/// the rest into a road that is fresh under every pass. With all of it in
/// the tread, a few seconds of understeer took a GT3's fronts from 85 to
/// 160 °C, the lost grip made it slide more, and a locked wheel cooked its
/// tread to 370 °C in six seconds.
const FRICTION_HEAT_TO_TYRE: f32 = 0.3;
/// Share of the rolling-resistance power that stays in the carcass as
/// heat; the rest goes into the tread and the road at the patch.
const CARCASS_HEAT_SHARE: f32 = 0.8;
/// Extra tread heat in dirty air, as a share of the gripping power per
/// unit of downforce the wake took (`TyreWork::wake_loss`): a quarter of
/// the downforce gone puts 7% more of the corner's work into the surface.
pub const DIRTY_AIR_SLIDE_HEAT: f32 = 0.3;

/// Where the patch's heat lands across the tread ([`zone_shares`]): the
/// shift toward the inner shoulder per radian the wheel's top leans toward
/// the car (3° of negative camber is a sixth of the heat moved), the shift
/// toward the outer shoulder per g of lateral force toward the car (the
/// carcass rolling onto it), and the share the middle gains per unit of
/// pressure error over the set pressure (10% over crowns it 15% more).
pub const ZONE_CAMBER_PER_RAD: f32 = 3.0;
pub const ZONE_FORCE_SHARE: f32 = 0.25;
pub const ZONE_PRESSURE_SHARE: f32 = 1.5;
const ZONE_MIN_SHARE: f32 = 0.05;

/// What a compound is for: a slick, or a treaded tyre for the rain.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum CompoundKind {
    #[default]
    Slick,
    Intermediate,
    Wet,
}

/// A tyre compound, as a change to the car's own tyre: its grip, how fast
/// it wears and where its working window sits. The reference compound is
/// the car's tyre exactly, so everything calibrated on it holds.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct Compound {
    pub name: String,
    #[serde(default)]
    pub kind: CompoundKind,
    /// Multiplier on the tyre's grip.
    pub grip_scale: f32,
    /// Multiplier on its wear.
    pub wear_scale: f32,
    /// Shift of its working window, °C (a soft works cooler).
    pub optimal_shift_c: f32,
    /// What it keeps of the road's grip on a dry road, in light rain and in
    /// heavy rain (`TrackSurface::water` 0, 0.5, 1; linear between). The
    /// road's own grip is baked for the tyre the weather calls for, so
    /// that tyre reads 1.0 there: a slick in heavy rain aquaplanes, a wet
    /// on a dry road is soft, treaded rubber that squirms.
    pub water_grip: [f32; 3],
    /// The compound the car was calibrated on: the setup knob's zero and
    /// what a car is fitted with unless someone chose otherwise.
    #[serde(default)]
    pub reference: bool,
}

impl Compound {
    fn new(
        name: &str,
        kind: CompoundKind,
        grip_scale: f32,
        wear_scale: f32,
        optimal_shift_c: f32,
        water_grip: [f32; 3],
        reference: bool,
    ) -> Self {
        Self {
            name: name.to_string(),
            kind,
            grip_scale,
            wear_scale,
            optimal_shift_c,
            water_grip,
            reference,
        }
    }

    /// Its grip on a road with this much water (0 dry .. 1 heavy rain, past
    /// 1 standing water: a puddle), as a multiplier on the car's tyre.
    pub fn grip_on(&self, water: f32) -> f32 {
        let w = water.clamp(0.0, 1.0) * 2.0;
        let [dry, light, heavy] = self.water_grip;
        let wet = if w <= 1.0 {
            dry + (light - dry) * w
        } else {
            light + (heavy - light) * (w - 1.0)
        };
        // Standing water deeper than the rain leaves: every tyre floats a
        // little.
        let puddle = 1.0 / (1.0 + PUDDLE_GRIP_LOSS * (water - 1.0).max(0.0));
        self.grip_scale * wet * puddle
    }
}

/// Grip lost per unit of water over 1 (a puddle: standing water deeper
/// than heavy rain leaves on a flat road).
pub const PUDDLE_GRIP_LOSS: f32 = 0.6;

/// A slick on a wet road: damp is a few seconds a lap, standing water is
/// aquaplaning.
const SLICK_WATER_GRIP: [f32; 3] = [1.0, 0.85, 0.6];

/// The compounds a car has when its car.toml lists none, softest slick
/// first, then the intermediate and the full wet: `CarState::tyre_compound`
/// and the setup's `tyre_compound` knob index them.
pub fn default_compounds() -> Vec<Compound> {
    vec![
        Compound::new(
            "soft",
            CompoundKind::Slick,
            1.03,
            1.8,
            -6.0,
            SLICK_WATER_GRIP,
            false,
        ),
        Compound::new(
            "medium",
            CompoundKind::Slick,
            1.0,
            1.0,
            0.0,
            SLICK_WATER_GRIP,
            true,
        ),
        Compound::new(
            "hard",
            CompoundKind::Slick,
            0.97,
            0.55,
            6.0,
            SLICK_WATER_GRIP,
            false,
        ),
        Compound::new(
            "intermediate",
            CompoundKind::Intermediate,
            1.0,
            1.5,
            -25.0,
            [0.88, 1.0, 0.88],
            false,
        ),
        Compound::new(
            "wet",
            CompoundKind::Wet,
            1.0,
            2.0,
            -35.0,
            [0.78, 0.95, 1.0],
            false,
        ),
    ]
}

/// The default list's indices: the medium (the reference), the intermediate
/// and the wet.
pub const MEDIUM: u8 = 1;
pub const INTERMEDIATE: u8 = 3;
pub const WET: u8 = 4;

/// The default list, built once.
fn defaults() -> &'static [Compound] {
    static LIST: std::sync::OnceLock<Vec<Compound>> = std::sync::OnceLock::new();
    LIST.get_or_init(default_compounds)
}

/// The default list's compound at `index`, the medium for one out of range.
pub fn compound(index: u8) -> &'static Compound {
    defaults()
        .get(index as usize)
        .unwrap_or(&defaults()[MEDIUM as usize])
}

/// The default list's tyre for a road with this much `water`: the
/// intermediate in light rain, the full wet past it; `None` when dry.
pub fn weather_compound(water: f32) -> Option<u8> {
    weather_compound_of(defaults(), water)
}

/// The tyre `compounds` holds for a road with this much `water`: the first
/// intermediate below [`WET_FROM_WATER`], the first wet from it; `None`
/// when dry, or when the car has no such tyre (a car with slicks alone
/// races on them in the rain).
pub fn weather_compound_of(compounds: &[Compound], water: f32) -> Option<u8> {
    if water < SLICK_BELOW_WATER {
        return None;
    }
    let want = if water < WET_FROM_WATER {
        CompoundKind::Intermediate
    } else {
        CompoundKind::Wet
    };
    let first = |kind| {
        compounds
            .iter()
            .position(|c| c.kind == kind)
            .map(|i| i as u8)
    };
    first(want).or_else(|| {
        // No tyre of that kind: the other treaded one, if any.
        match want {
            CompoundKind::Intermediate => first(CompoundKind::Wet),
            _ => first(CompoundKind::Intermediate),
        }
    })
}

/// Water from which the full wet is the tyre to be on.
pub const WET_FROM_WATER: f32 = 0.75;
/// Under this much water the road is a slick's: the damp a drying track
/// leaves (`crate::road_state`) is no reason for a treaded tyre.
pub const SLICK_BELOW_WATER: f32 = 0.1;

impl TireConfig {
    /// The car's compounds: its own list, or the default one.
    pub fn compounds(&self) -> &[Compound] {
        if self.compounds.is_empty() {
            defaults()
        } else {
            &self.compounds
        }
    }

    /// The compound at `index`, the reference for an index out of range.
    pub fn compound(&self, index: u8) -> &Compound {
        let list = self.compounds();
        list.get(index as usize)
            .unwrap_or(&list[self.reference_compound() as usize])
    }

    /// The index of the compound the car was calibrated on: the one marked
    /// `reference`, else the middle slick, else the first.
    pub fn reference_compound(&self) -> u8 {
        let list = self.compounds();
        if let Some(i) = list.iter().position(|c| c.reference) {
            return i as u8;
        }
        let slicks: Vec<usize> = list
            .iter()
            .enumerate()
            .filter(|(_, c)| c.kind == CompoundKind::Slick)
            .map(|(i, _)| i)
            .collect();
        slicks.get(slicks.len() / 2).copied().unwrap_or(0) as u8
    }

    /// The tyre the weather calls for on this car ([`weather_compound_of`]).
    pub fn weather_compound(&self, water: f32) -> Option<u8> {
        weather_compound_of(self.compounds(), water)
    }

    /// The compound the setup knob's `click` picks: the reference at 0, the
    /// softer (earlier) compounds for positive clicks, the harder and the
    /// treaded ones for negative, held to the list.
    pub fn compound_for_click(&self, click: i8) -> u8 {
        let last = self.compounds().len() as i32 - 1;
        (self.reference_compound() as i32 - click as i32).clamp(0, last.max(0)) as u8
    }

    /// `index` held to the list.
    pub fn valid_compound(&self, index: u8) -> u8 {
        if (index as usize) < self.compounds().len() {
            index
        } else {
            self.reference_compound()
        }
    }
}

/// Wear, percent of the tread, per megajoule the patch dissipates sliding
/// (the same friction power that heats it), on a medium at its window.
pub const WEAR_PERCENT_PER_MJ: f32 = 7.0;
/// Each degree the tread runs over its window wears it this much faster.
const HOT_WEAR_PER_C: f32 = 0.04;
/// Grip a tyre loses as it wears: this share across its life, and on top
/// of it a cliff of [`WEAR_CLIFF_LOSS`] from [`WEAR_CLIFF_START`] to worn
/// through (quadratic, so it comes on slowly and then all at once).
const WEAR_LINEAR_LOSS: f32 = 0.06;
const WEAR_CLIFF_START: f32 = 0.7;
const WEAR_CLIFF_LOSS: f32 = 0.24;

/// A wheel turning this much slower than the road (slip ratio, braking) is
/// locked: one patch of its tread scrubs the road and the rest rests.
pub const LOCK_SLIP_RATIO: f32 = 0.9;
/// Flat spot depth (0..1) per megajoule a locked wheel's patch dissipates,
/// on a medium: a second locked from 50 m/s on a loaded front (some
/// 0.2 MJ) leaves a third of the way to the worst.
const FLAT_SPOT_PER_MJ: f32 = 1.5;
/// Below this the wheel is crawling, m/s: nothing to grind.
const FLAT_SPOT_MIN_SPEED_MPS: f32 = 3.0;
/// Grip the deepest flat spot costs: the patch is not round any more and
/// rides off the road once a turn.
const FLAT_SPOT_GRIP_LOSS: f32 = 0.04;
/// A flat spot wears round again as the tread around it wears down to
/// it: this much of its depth per percent of tread worn, so the worst
/// spot is gone a third of the way through the tyre's life.
pub const FLAT_SPOT_ROUNDING_PER_PERCENT: f32 = 0.03;
/// What a punctured tyre keeps of its grip, on its carcass.
pub const PUNCTURE_GRIP: f32 = 0.35;
/// The pressure a punctured tyre reads, kPa.
pub const PUNCTURED_KPA: f32 = 15.0;
/// A tyre whose gauge pressure has leaked below this, kPa, is flat.
pub const PUNCTURE_FLAT_KPA: f32 = 60.0;
/// Rolling resistance of a punctured tyre, as a share of its load: a flat
/// tyre's carcass and rim drag on the road.
pub const PUNCTURE_ROLLING_RESISTANCE: f32 = 0.08;

/// Grip left in a tyre `wear_percent` worn.
pub fn wear_grip_factor(wear_percent: f32) -> f32 {
    let w = (wear_percent / 100.0).clamp(0.0, 1.0);
    let cliff = ((w - WEAR_CLIFF_START) / (1.0 - WEAR_CLIFF_START)).max(0.0);
    1.0 - WEAR_LINEAR_LOSS * w - WEAR_CLIFF_LOSS * cliff * cliff
}

/// What one tyre did this tick, as the heat model needs it.
#[derive(Debug, Clone, Copy, Default)]
pub struct TyreWork {
    /// Longitudinal and lateral force at the patch, N.
    pub fx: f32,
    pub fy: f32,
    pub slip_ratio: f32,
    pub slip_angle_rad: f32,
    /// Vertical load, N.
    pub load_n: f32,
    /// Speed of the wheel along the road, m/s.
    pub speed_mps: f32,
    /// Speed of the air over the tyre, m/s: the airflow through the wind
    /// and the wake of a car ahead, which is what cools it (the road under
    /// it arrives at `speed_mps`).
    pub air_mps: f32,
    /// How far the wheel's top leans toward the car, rad (negative camber
    /// against the road): where the inner shoulder meets the road.
    pub inner_lean_rad: f32,
    /// The lateral force toward the car over the load (g): the carcass
    /// rolling the patch onto the outer shoulder.
    pub outer_push: f32,
    /// The downforce the wake of a car ahead took from this axle, 0..1.
    pub wake_loss: f32,
    /// Water on the road under this tyre (`TrackSurface::water`, locally).
    pub water: f32,
}

impl TyreWork {
    /// Power the contact patch dissipates, W: each force times the speed
    /// the rubber slides at in its direction.
    pub fn slide_power_w(&self) -> f32 {
        let v = self.speed_mps.abs();
        self.fx.abs() * self.slip_ratio.abs().min(1.0) * v
            + self.fy.abs() * self.slip_angle_rad.sin().abs() * v
    }

    /// The patch's friction power split at the tyre's peak slip, W: the
    /// part up to it (the rubber gripping and deforming) and the part past
    /// it (the rubber sliding). They add up to [`Self::slide_power_w`].
    pub fn power_split_w(&self, tire: &TireConfig) -> (f32, f32) {
        let v = self.speed_mps.abs();
        let along = self.slip_ratio.abs().min(1.0);
        let across = self.slip_angle_rad.sin().abs();
        let along_peak = along.min(tire.optimal_slip_ratio.max(0.0));
        let across_peak = across.min(tire.optimal_slip_angle_rad.max(0.0).sin());
        let fx = self.fx.abs() * v;
        let fy = self.fy.abs() * v;
        let gripping = fx * along_peak + fy * across_peak;
        let sliding = fx * (along - along_peak) + fy * (across - across_peak);
        (gripping, sliding)
    }
}

/// Where the patch's heat lands across the tread, `[inner, middle, outer]`
/// shares summing to 1: evenly for an upright, square tyre at its set
/// pressure; toward the inner shoulder with the wheel's top leaning in
/// (`inner_lean_rad`), toward the outer with the carcass pushed onto it
/// (`outer_push`, g of lateral force toward the car), onto the middle
/// with the tyre crowned by over-inflation (`pressure_error`, a share of
/// the set pressure).
pub fn zone_shares(inner_lean_rad: f32, outer_push: f32, pressure_error: f32) -> [f32; 3] {
    let shift =
        (ZONE_CAMBER_PER_RAD * inner_lean_rad - ZONE_FORCE_SHARE * outer_push).clamp(-0.25, 0.25);
    let middle = (1.0 / 3.0 + ZONE_PRESSURE_SHARE * pressure_error).clamp(0.15, 0.6);
    let rest = 1.0 - middle;
    let inner = (rest / 2.0 + shift).max(ZONE_MIN_SHARE);
    let outer = (rest / 2.0 - shift).max(ZONE_MIN_SHARE);
    let sum = inner + middle + outer;
    [inner / sum, middle / sum, outer / sum]
}

/// Grip the temperature leaves a tyre whose tread and core are at these,
/// as a share of its peak (1.0 inside the window).
pub fn thermal_grip_factor(
    tire: &TireConfig,
    compound: &Compound,
    tread_c: f32,
    core_c: f32,
) -> f32 {
    let t = TREAD_GRIP_SHARE * tread_c + (1.0 - TREAD_GRIP_SHARE) * core_c;
    let from_optimum = t - optimum_c(tire, compound);
    let off = from_optimum.abs() - tire.temperature_window_c.max(0.0);
    if off <= 0.0 {
        return 1.0;
    }
    let eased = off * off / (off + FALLOFF_EASE_C);
    let falloff = tire.temperature_grip_falloff.max(0.0)
        * if from_optimum < 0.0 {
            COLD_FALLOFF_SHARE
        } else {
            1.0
        };
    (1.0 - falloff * eased).max(1.0 - MAX_THERMAL_GRIP_LOSS)
}

/// The middle of a compound's window on this car's tyre, °C.
pub fn optimum_c(tire: &TireConfig, compound: &Compound) -> f32 {
    tire.optimal_temperature_c + compound.optimal_shift_c
}

/// Gauge pressure, kPa, of a tyre set to `set_kpa` at its optimum whose gas
/// is at `core_c`: the gas law at constant volume, on absolute pressures.
pub fn pressure_kpa(tire: &TireConfig, compound: &Compound, set_kpa: f32, core_c: f32) -> f32 {
    let optimum = optimum_c(tire, compound);
    if core_c == optimum {
        return set_kpa;
    }
    let ratio = (core_c + KELVIN).max(1.0) / (optimum + KELVIN).max(1.0);
    (set_kpa + ATMOSPHERE_KPA) * ratio - ATMOSPHERE_KPA
}

/// The multiplier a tyre's grip takes from its compound, its wear, its
/// temperature and the pressure that puts it at (less what it has leaked).
/// A new medium at its optimum with the set pressure on it is the set
/// pressure's own factor, exactly.
pub fn grip_multiplier(
    tire: &TireConfig,
    compound: &Compound,
    set_kpa: f32,
    tyre: &TireData,
    water: f32,
) -> f32 {
    let optimum = optimum_c(tire, compound);
    let used = compound.grip_on(water)
        * wear_grip_factor(tyre.wear_percent)
        * (1.0 - FLAT_SPOT_GRIP_LOSS * tyre.flat_spot.clamp(0.0, 1.0));
    if tyre.punctured {
        return PUNCTURE_GRIP * tire.pressure_grip_factor(set_kpa) * used;
    }
    if tyre.core_temperature_c == optimum
        && tyre.temperature_c == optimum
        && tyre.pressure_loss_kpa == 0.0
    {
        return tire.pressure_grip_factor(set_kpa) * used;
    }
    thermal_grip_factor(tire, compound, tyre.temperature_c, tyre.core_temperature_c)
        * tire.pressure_grip_factor(
            pressure_kpa(tire, compound, set_kpa, tyre.core_temperature_c) - tyre.pressure_loss_kpa,
        )
        * used
}

/// The set pressure of each tyre, FL FR RL RR.
fn set_pressures(tire: &TireConfig) -> [f32; 4] {
    [
        tire.pressure_front_kpa,
        tire.pressure_front_kpa,
        tire.pressure_rear_kpa,
        tire.pressure_rear_kpa,
    ]
}

/// Refresh the pressure and the grip share a tyre reports from its
/// temperatures, wear and leaks. The grip share is against a new medium in
/// its window at its set pressure, so 1.0 is "as good as the setup makes
/// it" (a fresh soft is a little over). A tyre leaked flat punctures.
fn refresh(tyre: &mut TireData, tire: &TireConfig, compound: &Compound, set_kpa: f32, water: f32) {
    if !tyre.punctured {
        let pressure =
            pressure_kpa(tire, compound, set_kpa, tyre.core_temperature_c) - tyre.pressure_loss_kpa;
        if tyre.pressure_loss_kpa > 0.0 && pressure < PUNCTURE_FLAT_KPA {
            tyre.punctured = true;
        } else {
            tyre.pressure_kpa = pressure;
        }
    }
    if tyre.punctured {
        tyre.pressure_kpa = PUNCTURED_KPA;
    }
    let best = tire.pressure_grip_factor(set_kpa).max(1e-3);
    tyre.grip_factor = grip_multiplier(tire, compound, set_kpa, tyre, water) / best;
}

/// Put a new set of `compound` (an index into the car's compounds) on a
/// car, at `temperature_c` right through, as it is sent out on them, onto
/// a road with this much `water`.
pub fn fit(
    state: &mut CarState,
    tire: &TireConfig,
    temperature_c: f32,
    compound_index: u8,
    water: f32,
) {
    let index = tire.valid_compound(compound_index);
    let c = tire.compound(index);
    for (tyre, set) in state.tires.each_mut().into_iter().zip(set_pressures(tire)) {
        tyre.temperature_c = temperature_c;
        tyre.tread_c = [temperature_c; 3];
        tyre.core_temperature_c = temperature_c;
        tyre.wear_percent = 0.0;
        tyre.flat_spot = 0.0;
        tyre.punctured = false;
        tyre.leak_kpa_per_s = 0.0;
        tyre.pressure_loss_kpa = 0.0;
        refresh(tyre, tire, c, set, water);
    }
    state.tyre_compound = index;
    state.tyres_fitted = true;
}

/// Puncture a tyre: at once, or as a slow leak of `leak_kpa_per_s` that
/// the pressure curve charges for until the tyre is flat.
pub fn puncture(tyre: &mut TireData, leak_kpa_per_s: Option<f32>) {
    match leak_kpa_per_s {
        Some(rate) if rate > 0.0 => {
            tyre.leak_kpa_per_s = tyre.leak_kpa_per_s.max(rate);
        }
        _ => {
            tyre.punctured = true;
            tyre.pressure_kpa = PUNCTURED_KPA;
        }
    }
}

/// What a car's tyres are at when it is sent out: its blankets, or the air
/// when it has none (a blanket is never colder than the air).
pub fn start_temperature_c(tire: &TireConfig, surface: &TrackSurface) -> f32 {
    let air = surface.air_temperature_c;
    tire.blanket_temperature_c.map_or(air, |b| b.max(air))
}

/// Share of the way from [`start_temperature_c`] to the optimum a race
/// grid's tyres are at. There is no formation lap to drive, so the grid is
/// given the warmth one puts in: a car without blankets starts the race on
/// tyres that are cool rather than stone cold.
pub const FORMATION_LAP_WARMTH: f32 = 0.5;

/// What a race grid's tyres are at when the lights go out.
pub fn grid_temperature_c(tire: &TireConfig, surface: &TrackSurface) -> f32 {
    let start = start_temperature_c(tire, surface);
    let optimum = tire.optimal_temperature_c;
    // (The medium's optimum: a grid warms the tyres a formation lap's worth
    // whatever they are.)
    if start >= optimum {
        start
    } else {
        start + FORMATION_LAP_WARMTH * (optimum - start)
    }
}

/// Advance one tyre by `dt`: its heat across the three zones of its tread
/// and in its core, the tread its sliding wears away (faster the hotter it
/// runs over its window), its flat spot, its leak.
pub fn step(
    tyre: &mut TireData,
    work: &TyreWork,
    tire: &TireConfig,
    compound: &Compound,
    set_kpa: f32,
    surface: &TrackSurface,
    dt: f32,
) {
    let v = work.speed_mps.abs();
    let (gripping, sliding) = work.power_split_w(tire);
    let slide = gripping + sliding;
    let rolling = tire.rolling_resistance.max(0.0) * work.load_n.max(0.0) * v * CARCASS_HEAT_SHARE;

    let air = surface.air_temperature_c;
    let road = surface.track_temperature_c;
    // A tyre from before the zones, or one never fitted: even.
    if tyre.tread_c == [0.0; 3] {
        tyre.tread_c = [tyre.temperature_c; 3];
    }
    let core = tyre.core_temperature_c;

    let to_air = TREAD_AIR_STILL + TREAD_AIR_FORCED * work.air_mps.abs().powf(0.8);
    let wet = if surface.wet || work.water > 0.0 {
        WET_ROAD_FACTOR
    } else {
        1.0
    };
    // The patch only touches the road with a load on it.
    let to_road = if work.load_n > 1.0 {
        TREAD_ROAD_CONDUCTANCE * wet * v.sqrt()
    } else {
        0.0
    };

    // Sliding friction heats the tread's surface (the tyre's share of it);
    // the rubber's hysteresis heats the rubber by mass, and the carcass's
    // flexing the bulk. Dirty air stirs the patch for more.
    let tread_heat = FRICTION_HEAT_TO_TYRE * sliding
        + TREAD_HYSTERESIS_SHARE * gripping
        + DIRTY_AIR_SLIDE_HEAT * work.wake_loss.clamp(0.0, 1.0) * gripping;
    let pressure_error = if set_kpa > 1.0 {
        (tyre.pressure_kpa - set_kpa) / set_kpa
    } else {
        0.0
    };
    let shares = zone_shares(work.inner_lean_rad, work.outer_push, pressure_error);
    let zone_capacity = TREAD_HEAT_CAPACITY / 3.0;
    let z = tyre.tread_c;
    let mut across_total = 0.0;
    for i in 0..3 {
        let neighbours = match i {
            0 => ZONE_CONDUCTANCE * (z[1] - z[0]),
            1 => ZONE_CONDUCTANCE * (z[0] - z[1]) + ZONE_CONDUCTANCE * (z[2] - z[1]),
            _ => ZONE_CONDUCTANCE * (z[1] - z[2]),
        };
        let across = TREAD_CORE_CONDUCTANCE / 3.0 * (z[i] - core);
        across_total += across;
        let zone_in =
            tread_heat * shares[i] - to_air / 3.0 * (z[i] - air) - to_road / 3.0 * (z[i] - road)
                + neighbours
                - across;
        tyre.tread_c[i] = z[i] + zone_in * dt / zone_capacity;
    }
    let core_in = (1.0 - TREAD_HYSTERESIS_SHARE) * gripping + rolling
        - CORE_AIR_SHARE * to_air * (core - air);
    tyre.temperature_c = (tyre.tread_c[0] + tyre.tread_c[1] + tyre.tread_c[2]) / 3.0;
    tyre.core_temperature_c = core + (core_in + across_total) * dt / CORE_HEAT_CAPACITY;

    let tread = tyre.temperature_c;
    let over = (tread - optimum_c(tire, compound) - tire.temperature_window_c).max(0.0);
    let wear = WEAR_PERCENT_PER_MJ
        * slide
        * dt
        * 1e-6
        * compound.wear_scale
        * tire.wear_rate.max(0.0)
        * (1.0 + HOT_WEAR_PER_C * over);
    tyre.wear_percent = (tyre.wear_percent + wear).min(100.0);
    // Worn through, it lets go of its air.
    if tyre.wear_percent >= 100.0 {
        tyre.punctured = true;
    }
    // The tread around a flat spot wears down to it ...
    tyre.flat_spot = (tyre.flat_spot - wear * FLAT_SPOT_ROUNDING_PER_PERCENT).max(0.0);
    // ... and a locked wheel grinds one patch of its tread flat (softer
    // rubber faster).
    if work.slip_ratio <= -LOCK_SLIP_RATIO && v > FLAT_SPOT_MIN_SPEED_MPS {
        let grind = work.fx.abs() * work.slip_ratio.abs().min(1.0) * v;
        tyre.flat_spot =
            (tyre.flat_spot + FLAT_SPOT_PER_MJ * grind * dt * 1e-6 * compound.wear_scale).min(1.0);
    }
    // A slow puncture lets the air out as it rolls.
    if tyre.leak_kpa_per_s > 0.0 && !tyre.punctured {
        tyre.pressure_loss_kpa += tyre.leak_kpa_per_s * dt;
    }
    refresh(tyre, tire, compound, set_kpa, work.water.max(surface.water));
}

/// Advance all four tyres, FL FR RL RR.
pub fn step_all(
    state: &mut CarState,
    work: &[TyreWork; 4],
    tire: &TireConfig,
    surface: &TrackSurface,
    dt: f32,
) {
    let c = tire.compound(state.tyre_compound);
    for ((tyre, w), set) in state
        .tires
        .each_mut()
        .into_iter()
        .zip(work)
        .zip(set_pressures(tire))
    {
        step(tyre, w, tire, c, set, surface, dt);
    }
}

impl CarState {
    /// How much of its grip the car's weaker axle has from its tyres'
    /// compound, wear, temperatures and pressures (1.0 a new medium in the
    /// window at the set pressure), and from the road under it against the
    /// road the session was planned on (`surface_grip_share`: a drying
    /// line, a puddle): what a driver who can feel the tyres drives to.
    /// 1.0 before the tyres are fitted.
    pub fn tyre_grip_share(&self) -> f32 {
        if !self.tyres_fitted {
            return 1.0;
        }
        let t = &self.tires;
        let front = 0.5 * (t.front_left.grip_factor + t.front_right.grip_factor);
        let rear = 0.5 * (t.rear_left.grip_factor + t.rear_right.grip_factor);
        // A fresh soft is a little over the plan's tyre, and a driver uses it.
        let tyres = front.min(rear).clamp(0.0, 1.03);
        tyres * self.surface_grip_share.clamp(0.0, 1.3)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn medium() -> Compound {
        compound(MEDIUM).clone()
    }

    fn tyre() -> TireConfig {
        TireConfig::default()
    }

    /// A tyre at `tread` and `core` right through, as a step's input.
    fn at(tread: f32, core: f32) -> TireData {
        TireData {
            temperature_c: tread,
            tread_c: [tread; 3],
            core_temperature_c: core,
            ..Default::default()
        }
    }

    #[test]
    fn the_window_is_flat_and_the_edges_ease_in() {
        let t = tyre();
        let m = medium();
        let opt = t.optimal_temperature_c;
        for d in [-10.0, -5.0, 0.0, 5.0, 10.0] {
            assert_eq!(thermal_grip_factor(&t, &m, opt + d, opt + d), 1.0);
        }
        // Just past the edge: almost nothing, then the full slope.
        let edge = 1.0 - thermal_grip_factor(&t, &m, opt + 11.0, opt + 11.0);
        assert!(
            edge > 0.0 && edge < t.temperature_grip_falloff * 0.3,
            "{edge}"
        );
        let far = |d: f32| thermal_grip_factor(&t, &m, opt + d, opt + d);
        let slope = far(40.0) - far(41.0);
        assert!((slope - t.temperature_grip_falloff).abs() < 2e-4, "{slope}");
        // Cold is charged less per degree: the pressure charges the rest.
        let cold = 1.0 - far(-30.0);
        let hot = 1.0 - far(30.0);
        assert!(
            (cold - COLD_FALLOFF_SHARE * hot).abs() < 1e-5,
            "{cold} {hot}"
        );
        // And never below the floor.
        assert_eq!(far(-400.0), 1.0 - MAX_THERMAL_GRIP_LOSS);
    }

    #[test]
    fn the_tread_counts_more_than_the_core() {
        let t = tyre();
        let m = medium();
        let opt = t.optimal_temperature_c;
        // 30 °C over is 9 over when it is the core, 21 when it is the tread.
        let hot_tread = thermal_grip_factor(&t, &m, opt + 30.0, opt);
        let hot_core = thermal_grip_factor(&t, &m, opt, opt + 30.0);
        assert!(hot_tread < hot_core && hot_core == 1.0);
    }

    #[test]
    fn the_set_pressure_is_the_pressure_at_the_optimum() {
        let t = tyre();
        let m = medium();
        let opt = t.optimal_temperature_c;
        assert!((pressure_kpa(&t, &m, 175.0, opt) - 175.0).abs() < 1e-3);
        // A slick set to 180 hot is at about 125 kPa at 20 °C, as a real
        // tyre that gains half a bar on the way up to temperature.
        let cold = pressure_kpa(&t, &m, 180.0, 20.0);
        assert!((120.0..132.0).contains(&cold), "{cold}");
        assert!(pressure_kpa(&t, &m, 180.0, opt + 20.0) > 195.0);
    }

    #[test]
    fn a_tyre_at_its_optimum_grips_as_it_did_before_temperatures() {
        let t = tyre();
        let m = medium();
        let mut data = at(t.optimal_temperature_c, t.optimal_temperature_c);
        assert_eq!(grip_multiplier(&t, &m, 180.0, &data, 0.0), 1.0);
        assert_eq!(
            grip_multiplier(&t, &m, 190.0, &data, 0.0),
            t.pressure_grip_factor(190.0)
        );
        refresh(&mut data, &t, &m, 190.0, 0.0);
        assert_eq!(data.grip_factor, 1.0, "as good as the setup makes it");
        assert_eq!(data.pressure_kpa, 190.0);
    }

    #[test]
    fn a_slide_past_the_peak_puts_most_of_its_heat_into_the_road() {
        let t = tyre();
        let m = medium();
        let surface = TrackSurface::default();
        let heat = |work: &TyreWork| {
            let mut data = at(85.0, 70.0);
            for _ in 0..(5 * 240) {
                step(&mut data, work, &t, &m, 180.0, &surface, 1.0 / 240.0);
            }
            data.temperature_c
        };
        // The split adds up to the whole.
        let understeer = TyreWork {
            fy: 4000.0,
            slip_angle_rad: 0.5,
            load_n: 3300.0,
            speed_mps: 25.0,
            air_mps: 25.0,
            ..Default::default()
        };
        let (gripping, sliding) = understeer.power_split_w(&t);
        assert!((gripping + sliding - understeer.slide_power_w()).abs() < 1.0);
        assert!(sliding > 2.0 * gripping);
        // Five seconds of heavy understeer warms the fronts, not cooks them.
        let hot = heat(&understeer);
        assert!((95.0..135.0).contains(&hot), "understeer: {hot:.1}");
        // Nor does five seconds of a locked wheel at 40 m/s.
        let locked = TyreWork {
            fx: 4000.0,
            slip_ratio: -1.0,
            load_n: 3300.0,
            speed_mps: 40.0,
            air_mps: 40.0,
            ..Default::default()
        };
        let hot = heat(&locked);
        assert!(hot < 220.0, "locked: {hot:.1}");
        // At the peak nothing changes: all of it is gripping power.
        let at_peak = TyreWork {
            fy: 4000.0,
            slip_angle_rad: t.optimal_slip_angle_rad,
            load_n: 3300.0,
            speed_mps: 25.0,
            air_mps: 25.0,
            ..Default::default()
        };
        assert!(at_peak.power_split_w(&t).1.abs() < 1e-3);
    }

    #[test]
    fn sliding_heats_the_tread_and_a_straight_cools_it() {
        let t = tyre();
        let m = medium();
        let surface = TrackSurface::default();
        let mut data = at(80.0, 80.0);
        // Twice the peak slip angle: a proper slide.
        let slide = TyreWork {
            fy: 6_000.0,
            slip_angle_rad: 0.25,
            load_n: 5_000.0,
            speed_mps: 40.0,
            air_mps: 40.0,
            ..Default::default()
        };
        for _ in 0..(240 * 2) {
            step(&mut data, &slide, &t, &m, 180.0, &surface, 1.0 / 240.0);
        }
        assert!(
            data.temperature_c > data.core_temperature_c + 2.0,
            "the tread runs ahead: {data:?}"
        );
        for _ in 0..(240 * 4) {
            step(&mut data, &slide, &t, &m, 180.0, &surface, 1.0 / 240.0);
        }
        assert!(data.temperature_c > 95.0, "{data:?}");
        let peak = data.temperature_c;
        let straight = TyreWork {
            load_n: 4_000.0,
            speed_mps: 80.0,
            air_mps: 80.0,
            ..Default::default()
        };
        for _ in 0..(240 * 5) {
            step(&mut data, &straight, &t, &m, 180.0, &surface, 1.0 / 240.0);
        }
        assert!(data.temperature_c < peak - 8.0, "{peak} -> {data:?}");
    }

    /// The report that found the old split: an LMP2 driven carefully on a
    /// pad, 2° of slip on a loaded front at 70 m/s (a third of the peak;
    /// 22 kW of slip power), gained 10 °C per slight turn and 65 through
    /// Eau Rouge. A fast corner now warms the surface by a few degrees,
    /// and a lap of them does not run away.
    #[test]
    fn a_careful_corner_warms_the_tread_a_few_degrees() {
        let t = tyre();
        let m = medium();
        let surface = TrackSurface::default();
        let mut data = at(92.0, 92.0);
        let corner = TyreWork {
            fy: 9_000.0,
            slip_angle_rad: 2.0f32.to_radians(),
            load_n: 6_500.0,
            speed_mps: 70.0,
            air_mps: 70.0,
            ..Default::default()
        };
        assert!((corner.slide_power_w() - 22_000.0).abs() < 500.0);
        for _ in 0..(240 * 4) {
            step(&mut data, &corner, &t, &m, 180.0, &surface, 1.0 / 240.0);
        }
        let rise = data.temperature_c - 92.0;
        assert!((1.0..12.0).contains(&rise), "four seconds of it: {data:?}");
        // The bulk takes most of the energy, and shows it slowly.
        let core_rise = data.core_temperature_c - 92.0;
        assert!((2.0..12.0).contains(&core_rise), "{data:?}");
        assert!(
            rise < 2.0 * core_rise,
            "the surface leads a little: {data:?}"
        );
        // The straight after it takes the surface back toward the bulk.
        let peak = data.temperature_c;
        let straight = TyreWork {
            load_n: 5_000.0,
            speed_mps: 75.0,
            air_mps: 75.0,
            ..Default::default()
        };
        for _ in 0..(240 * 5) {
            step(&mut data, &straight, &t, &m, 180.0, &surface, 1.0 / 240.0);
        }
        assert!(data.temperature_c < peak - 2.0, "{peak} -> {data:?}");
        // (A lap of such corners is pinned on the AI at Monza and Spa in
        // `tests/tyre_temperature_test.rs`.)
    }

    /// The surface layer against a real tyre, not only the AI's windows: a
    /// GT slick's surface, read by a pyrometer or an infrared camera,
    /// rises 15-35 °C through a hard corner and gives most of it back down
    /// the straight after it, while the bulk moves a few degrees over the
    /// lap. A synthetic lap of six such corners and straights, at a GT3's
    /// slip power.
    #[test]
    fn the_surface_swings_like_a_real_tyre_over_a_lap() {
        let t = tyre();
        let m = medium();
        let surface = TrackSurface::default();
        let mut data = at(90.0, 88.0);
        // A hard corner: a loaded outside front at 1.3 g, a little past the
        // peak slip, for three seconds at 35 m/s; then twelve seconds of
        // straight at 65 m/s.
        let corner = TyreWork {
            fy: 6_000.0,
            slip_angle_rad: 1.2 * t.optimal_slip_angle_rad,
            load_n: 5_000.0,
            speed_mps: 35.0,
            air_mps: 35.0,
            ..Default::default()
        };
        let straight = TyreWork {
            load_n: 4_000.0,
            speed_mps: 65.0,
            air_mps: 65.0,
            ..Default::default()
        };
        let dt = 1.0 / 420.0;
        let (mut swing_min, mut swing_max) = (f32::MAX, f32::MIN);
        let (mut core_min, mut core_max) = (f32::MAX, f32::MIN);
        for lap in 0..3 {
            for _ in 0..6 {
                for _ in 0..(420 * 3) {
                    step(&mut data, &corner, &t, &m, 180.0, &surface, dt);
                    if lap == 2 {
                        swing_max = swing_max.max(data.temperature_c);
                        core_max = core_max.max(data.core_temperature_c);
                    }
                }
                for _ in 0..(420 * 12) {
                    step(&mut data, &straight, &t, &m, 180.0, &surface, dt);
                    if lap == 2 {
                        swing_min = swing_min.min(data.temperature_c);
                        core_min = core_min.min(data.core_temperature_c);
                    }
                }
            }
        }
        let swing = swing_max - swing_min;
        let core_swing = core_max - core_min;
        println!(
            "surface {swing_min:.1}-{swing_max:.1} (swing {swing:.1}), core {core_min:.1}-{core_max:.1} (swing {core_swing:.1})"
        );
        assert!((12.0..40.0).contains(&swing), "surface swing {swing:.1}");
        // (The bulk follows at about six tenths of the surface's swing on
        // this lap: the hysteresis heats it through, and it has nothing
        // but the air inside the wheel to cool it.)
        assert!(
            core_swing < 0.75 * swing,
            "the bulk moves less than the surface: {core_swing:.1} against {swing:.1}"
        );
        assert!(
            (60.0..130.0).contains(&swing_max),
            "a GT slick's surface at a corner's exit: {swing_max:.1}"
        );
    }

    #[test]
    fn a_parked_tyre_settles_toward_the_air_and_road() {
        let t = tyre();
        let m = medium();
        let surface = TrackSurface::default();
        let mut data = at(100.0, 100.0);
        let idle = TyreWork::default();
        let mut last = data.temperature_c;
        for _ in 0..30 {
            for _ in 0..(240 * 60) {
                step(&mut data, &idle, &t, &m, 180.0, &surface, 1.0 / 240.0);
            }
            assert!(data.temperature_c < last, "cooling every minute: {data:?}");
            last = data.temperature_c;
        }
        // Half an hour in still air: most of the way down, the core behind.
        let air = surface.air_temperature_c;
        assert!(data.temperature_c - air < 0.2 * (100.0 - air), "{data:?}");
        assert!(data.core_temperature_c > data.temperature_c);
    }

    #[test]
    fn a_wet_road_takes_more_heat() {
        let t = tyre();
        let m = medium();
        let dry = TrackSurface::default();
        let wet = TrackSurface {
            wet: true,
            ..TrackSurface::default()
        };
        let roll = TyreWork {
            load_n: 4_000.0,
            speed_mps: 50.0,
            air_mps: 50.0,
            ..Default::default()
        };
        let run = |surface: &TrackSurface| {
            let mut data = at(90.0, 90.0);
            for _ in 0..(240 * 10) {
                step(&mut data, &roll, &t, &m, 180.0, surface, 1.0 / 240.0);
            }
            data.temperature_c
        };
        assert!(run(&wet) < run(&dry) - 5.0);
    }

    #[test]
    fn blankets_send_a_car_out_warm() {
        let surface = TrackSurface::default();
        let mut t = tyre();
        assert_eq!(start_temperature_c(&t, &surface), surface.air_temperature_c);
        t.blanket_temperature_c = Some(70.0);
        assert_eq!(start_temperature_c(&t, &surface), 70.0);
        // The grid is half way from there to the optimum.
        assert_eq!(
            grid_temperature_c(&t, &surface),
            70.0 + FORMATION_LAP_WARMTH * (t.optimal_temperature_c - 70.0)
        );
    }

    #[test]
    fn slower_air_cools_less_so_a_tow_or_a_tailwind_runs_hotter() {
        let t = tyre();
        let m = medium();
        let surface = TrackSurface::default();
        let run = |air_mps: f32| {
            let mut data = at(95.0, 95.0);
            let roll = TyreWork {
                fy: 3_000.0,
                slip_angle_rad: 0.03,
                load_n: 4_000.0,
                speed_mps: 60.0,
                air_mps,
                ..Default::default()
            };
            for _ in 0..(240 * 30) {
                step(&mut data, &roll, &t, &m, 180.0, &surface, 1.0 / 240.0);
            }
            data.temperature_c
        };
        // Clean air at the car's speed, then 70% of it (a close tow).
        let clean = run(60.0);
        let towed = run(42.0);
        assert!(towed > clean + 1.0, "{clean:.1} -> {towed:.1}");
    }

    #[test]
    fn dirty_air_puts_more_of_the_corners_work_into_the_tread() {
        let t = tyre();
        let m = medium();
        let surface = TrackSurface::default();
        let run = |wake_loss: f32| {
            let mut data = at(90.0, 90.0);
            let corner = TyreWork {
                fy: 6_000.0,
                slip_angle_rad: 0.8 * t.optimal_slip_angle_rad,
                load_n: 5_000.0,
                speed_mps: 45.0,
                air_mps: 45.0,
                wake_loss,
                ..Default::default()
            };
            for _ in 0..(420 * 4) {
                step(&mut data, &corner, &t, &m, 180.0, &surface, 1.0 / 420.0);
            }
            data.temperature_c
        };
        let clean = run(0.0);
        let dirty = run(0.25);
        assert!(dirty > clean + 0.5, "{clean:.1} -> {dirty:.1}");
    }

    #[test]
    fn the_heat_lands_where_the_patch_is_and_an_even_tyre_is_one_node() {
        // Upright, square, at pressure: a third each.
        let even = zone_shares(0.0, 0.0, 0.0);
        for s in even {
            assert!((s - 1.0 / 3.0).abs() < 1e-6, "{even:?}");
        }
        // Negative camber: the inner shoulder.
        let cambered = zone_shares(3.0f32.to_radians(), 0.0, 0.0);
        assert!(
            cambered[0] > cambered[2] && cambered[0] > 0.4,
            "{cambered:?}"
        );
        // A lateral force toward the car: the outer shoulder.
        let pushed = zone_shares(0.0, 1.0, 0.0);
        assert!(pushed[2] > pushed[0], "{pushed:?}");
        // Over-inflated: the middle.
        let crowned = zone_shares(0.0, 0.0, 0.1);
        assert!(crowned[1] > 0.45, "{crowned:?}");
        for shares in [even, cambered, pushed, crowned] {
            let sum: f32 = shares.iter().sum();
            assert!((sum - 1.0).abs() < 1e-5);
            assert!(shares.iter().all(|s| *s >= ZONE_MIN_SHARE - 1e-6));
        }

        // Driven on its inner shoulder the inner zone runs hottest, and the
        // mean is what the grip reads.
        let t = tyre();
        let m = medium();
        let surface = TrackSurface::default();
        let mut data = at(90.0, 90.0);
        let corner = TyreWork {
            fy: 6_000.0,
            slip_angle_rad: 0.8 * t.optimal_slip_angle_rad,
            load_n: 5_000.0,
            speed_mps: 45.0,
            air_mps: 45.0,
            inner_lean_rad: 3.5f32.to_radians(),
            ..Default::default()
        };
        for _ in 0..(420 * 6) {
            step(&mut data, &corner, &t, &m, 180.0, &surface, 1.0 / 420.0);
        }
        let [inner, middle, outer] = data.tread_c;
        assert!(inner > middle && middle > outer, "{:?}", data.tread_c);
        assert!(
            inner - outer > 3.0,
            "a shoulder's worth: {:?}",
            data.tread_c
        );
        assert!(((inner + middle + outer) / 3.0 - data.temperature_c).abs() < 1e-3);
        // Even heat is the one-node model: the three zones stay together.
        let mut flat = at(90.0, 90.0);
        let square = TyreWork {
            inner_lean_rad: 0.0,
            ..corner
        };
        for _ in 0..(420 * 6) {
            step(&mut flat, &square, &t, &m, 180.0, &surface, 1.0 / 420.0);
        }
        assert!(
            (flat.tread_c[0] - flat.tread_c[2]).abs() < 1e-3,
            "{:?}",
            flat.tread_c
        );
    }

    #[test]
    fn a_locked_wheel_grinds_a_flat_spot_and_a_worn_tyre_lets_go() {
        let t = tyre();
        let m = medium();
        let surface = TrackSurface::default();
        let opt = t.optimal_temperature_c;
        let mut data = at(opt, opt);
        refresh(&mut data, &t, &m, 180.0, 0.0);
        let fresh = data.grip_factor;
        // Braking hard at the peak: no flat spot.
        let braking = TyreWork {
            fx: -6_000.0,
            slip_ratio: -t.optimal_slip_ratio,
            load_n: 6_000.0,
            speed_mps: 50.0,
            air_mps: 50.0,
            ..Default::default()
        };
        for _ in 0..240 {
            step(&mut data, &braking, &t, &m, 180.0, &surface, 1.0 / 240.0);
        }
        assert_eq!(data.flat_spot, 0.0);
        // A second locked from 50 m/s.
        let locked = TyreWork {
            slip_ratio: -1.0,
            ..braking
        };
        for _ in 0..240 {
            step(&mut data, &locked, &t, &m, 180.0, &surface, 1.0 / 240.0);
        }
        assert!(
            (0.2..0.7).contains(&data.flat_spot),
            "a second's lock: {}",
            data.flat_spot
        );
        let spotted = TireData {
            flat_spot: 1.0,
            ..at(opt, opt)
        };
        let loss = 1.0 - grip_multiplier(&t, &m, 180.0, &spotted, 0.0) / fresh;
        assert!((loss - FLAT_SPOT_GRIP_LOSS).abs() < 1e-4, "{loss}");

        // Worn through, the tyre punctures: most of its grip and its air go.
        data.wear_percent = 99.999;
        for _ in 0..240 {
            step(&mut data, &braking, &t, &m, 180.0, &surface, 1.0 / 240.0);
        }
        assert!(data.punctured);
        assert_eq!(data.pressure_kpa, PUNCTURED_KPA);
        assert!(data.grip_factor < 0.4, "{}", data.grip_factor);

        // A new set clears both.
        let mut car = CarState::new(
            uuid::Uuid::nil(),
            uuid::Uuid::nil(),
            &crate::data::GridSlot {
                position: 1,
                x: 0.0,
                y: 0.0,
                z: 0.0,
                yaw_rad: 0.0,
            },
        );
        car.tires.front_left = data;
        fit(&mut car, &t, opt, MEDIUM, 0.0);
        assert!(!car.tires.front_left.punctured);
        assert_eq!(car.tires.front_left.flat_spot, 0.0);
        assert_eq!(car.tires.front_left.tread_c, [opt; 3]);
    }

    #[test]
    fn a_flat_spot_wears_round_again() {
        let t = tyre();
        let m = medium();
        let surface = TrackSurface::default();
        let opt = t.optimal_temperature_c;
        let mut data = TireData {
            flat_spot: 0.6,
            ..at(opt, opt)
        };
        // Hard cornering at the peak: wear, no lock.
        let corner = TyreWork {
            fy: 7_000.0,
            slip_angle_rad: 1.3 * t.optimal_slip_angle_rad,
            load_n: 6_000.0,
            speed_mps: 45.0,
            air_mps: 45.0,
            ..Default::default()
        };
        for _ in 0..(420 * 60) {
            step(&mut data, &corner, &t, &m, 180.0, &surface, 1.0 / 420.0);
        }
        assert!(data.wear_percent > 1.0, "{}", data.wear_percent);
        let expected = (0.6 - data.wear_percent * FLAT_SPOT_ROUNDING_PER_PERCENT).max(0.0);
        assert!(
            (data.flat_spot - expected).abs() < 0.02,
            "{} vs {expected}",
            data.flat_spot
        );
        assert!(data.flat_spot < 0.6);
    }

    #[test]
    fn a_slow_puncture_leaks_to_flat_and_a_quick_one_is_flat_at_once() {
        let t = tyre();
        let m = medium();
        let surface = TrackSurface::default();
        let opt = t.optimal_temperature_c;
        let mut data = at(opt, opt);
        refresh(&mut data, &t, &m, 180.0, 0.0);
        let full = data.grip_factor;
        puncture(&mut data, Some(4.0));
        let roll = TyreWork {
            load_n: 4_000.0,
            speed_mps: 50.0,
            air_mps: 50.0,
            ..Default::default()
        };
        for _ in 0..(420 * 10) {
            step(&mut data, &roll, &t, &m, 180.0, &surface, 1.0 / 420.0);
        }
        assert!(!data.punctured, "ten seconds in: still rolling, softer");
        assert!(
            data.pressure_kpa < 150.0 && data.grip_factor < full,
            "{data:?}"
        );
        for _ in 0..(420 * 40) {
            step(&mut data, &roll, &t, &m, 180.0, &surface, 1.0 / 420.0);
        }
        assert!(data.punctured, "{data:?}");
        assert_eq!(data.pressure_kpa, PUNCTURED_KPA);
        let mut quick = at(opt, opt);
        puncture(&mut quick, None);
        assert!(quick.punctured);
    }

    #[test]
    fn the_weathers_tyre_grips_as_the_road_was_baked_and_slicks_aquaplane() {
        let medium = compound(MEDIUM);
        let inter = compound(INTERMEDIATE);
        let wet = compound(WET);
        // Dry: the medium is the car's tyre; the treaded tyres squirm.
        assert_eq!(medium.grip_on(0.0), 1.0);
        assert!(inter.grip_on(0.0) < 0.9 && wet.grip_on(0.0) < inter.grip_on(0.0));
        // Light rain is the intermediate's, heavy the wet's: exactly what
        // the road was baked for.
        assert_eq!(inter.grip_on(0.5), 1.0);
        assert_eq!(wet.grip_on(1.0), 1.0);
        assert!(inter.grip_on(0.5) > wet.grip_on(0.5));
        assert!(inter.grip_on(0.5) > medium.grip_on(0.5));
        // A slick in standing water is little better than a sledge, and a
        // puddle takes more off everyone.
        assert!(medium.grip_on(1.0) <= 0.6);
        assert!(wet.grip_on(1.5) < wet.grip_on(1.0));
        assert_eq!(weather_compound(0.0), None);
        assert_eq!(weather_compound(0.5), Some(INTERMEDIATE));
        assert_eq!(weather_compound(1.0), Some(WET));
    }

    #[test]
    fn a_car_may_list_its_own_compounds() {
        let mut t = tyre();
        assert_eq!(t.compounds().len(), 5);
        assert_eq!(t.reference_compound(), MEDIUM);
        assert_eq!(t.compound_for_click(1), 0);
        assert_eq!(t.compound_for_click(-3), WET);
        assert_eq!(t.compound_for_click(9), 0, "held to the list");
        t.compounds = vec![
            Compound::new(
                "supersoft",
                CompoundKind::Slick,
                1.05,
                2.5,
                -8.0,
                SLICK_WATER_GRIP,
                false,
            ),
            Compound::new(
                "prime",
                CompoundKind::Slick,
                1.0,
                1.0,
                0.0,
                SLICK_WATER_GRIP,
                false,
            ),
            Compound::new(
                "rain",
                CompoundKind::Wet,
                1.0,
                1.8,
                -30.0,
                [0.8, 0.95, 1.0],
                false,
            ),
        ];
        // No reference marked: the middle slick (of two, the harder).
        assert_eq!(t.reference_compound(), 1, "{:?}", t.compounds);
        t.compounds[1].reference = true;
        assert_eq!(t.reference_compound(), 1);
        assert_eq!(t.compound_for_click(1), 0);
        assert_eq!(t.compound_for_click(-1), 2);
        assert_eq!(t.compound_for_click(-5), 2);
        assert_eq!(t.compound(7).name, "prime", "out of range is the reference");
        // No intermediate: light rain calls for the wet.
        assert_eq!(t.weather_compound(0.5), Some(2));
        assert_eq!(t.weather_compound(1.0), Some(2));
        // Slicks alone: the rain calls for nothing.
        t.compounds.pop();
        assert_eq!(t.weather_compound(1.0), None);
        assert_eq!(t.valid_compound(5), 1);
    }
}
