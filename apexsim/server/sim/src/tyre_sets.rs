//! A car's sets of tyres across a session's runs, for the rules that count
//! them (`data::RaceRules`): the qualifying tyre allowance, a weekend's
//! tyre allocation and starting the race on the set a car qualified on.
//!
//! Without those rules every run goes out on a new set, as it always has,
//! and nothing here is kept.

use crate::data::{CarState, TireConfig};
use crate::tyre_thermal::CompoundKind;

/// One set of four tyres off the car: what it is and how far it has run.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct TyreSet {
    /// Its compound, within the car's own list.
    pub compound: u8,
    /// Each tyre's wear, flat spot, pick-up and blistering, FL FR RL RR.
    pub wear_percent: [f32; 4],
    pub flat_spot: [f32; 4],
    pub pickup: [f32; 4],
    pub blister: [f32; 4],
    /// Laps it has run, all its runs together.
    pub laps: f32,
}

impl TyreSet {
    /// The set on `state`'s car, run `laps` laps.
    pub fn of(state: &CarState, laps: f32) -> Self {
        let tyres = state.tires.each();
        Self {
            compound: state.tyre_compound,
            wear_percent: tyres.map(|t| t.wear_percent),
            flat_spot: tyres.map(|t| t.flat_spot),
            pickup: tyres.map(|t| t.pickup),
            blister: tyres.map(|t| t.blister),
            laps: laps.max(0.0),
        }
    }

    /// Put its wear back on tyres just fitted at its compound
    /// (`tyre_thermal::fit`), which fits them new.
    pub fn wear_onto(&self, state: &mut CarState) {
        for (i, tyre) in state.tires.each_mut().into_iter().enumerate() {
            tyre.wear_percent = self.wear_percent[i];
            tyre.flat_spot = self.flat_spot[i];
            tyre.pickup = self.pickup[i];
            tyre.blister = self.blister[i];
        }
    }

    /// The most worn tyre's wear, percent.
    pub fn worst_wear(&self) -> f32 {
        self.wear_percent.iter().copied().fold(0.0, f32::max)
    }
}

/// Whether `compound` is a slick: the sets the rules count. A treaded tyre
/// (intermediate, wet, snow) is never counted, so rain never strands a car.
pub fn counted(tire: &TireConfig, compound: u8) -> bool {
    tire.compound(compound).kind == CompoundKind::Slick
}

/// One car's sets.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct CarTyres {
    /// Used slick sets back in the garage, in the order they came off.
    pub kept: Vec<TyreSet>,
    /// New slick sets taken in this qualifying (every round of a weekend's).
    pub new_in_qualifying: u8,
    /// New slick sets taken in a weekend's practice and qualifying.
    pub new_in_weekend: u8,
    /// The laps the set on the car had run before it went on this run.
    pub on_car_laps: f32,
    /// The set on the car as it ended its last timed lap.
    pub lap_end: Option<TyreSet>,
    /// The set on the car when it set the lap that placed it in qualifying.
    pub qualifying: Option<TyreSet>,
}

impl CarTyres {
    /// The set to go out on when no new one may be taken: the least worn
    /// kept set of `compound`, else the least worn kept set. Taken out of
    /// the garage. `None` with nothing kept.
    pub fn take_used(&mut self, compound: u8) -> Option<TyreSet> {
        let least = |only: Option<u8>| {
            self.kept
                .iter()
                .enumerate()
                .filter(|(_, s)| only.is_none_or(|c| s.compound == c))
                .min_by(|(ia, a), (ib, b)| {
                    a.worst_wear().total_cmp(&b.worst_wear()).then(ia.cmp(ib))
                })
                .map(|(i, _)| i)
        };
        let index = least(Some(compound)).or_else(|| least(None))?;
        Some(self.kept.remove(index))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn set(compound: u8, wear: f32) -> TyreSet {
        TyreSet {
            compound,
            wear_percent: [wear; 4],
            flat_spot: [0.0; 4],
            pickup: [0.0; 4],
            blister: [0.0; 4],
            laps: 1.0,
        }
    }

    #[test]
    fn the_least_worn_set_of_the_compound_goes_back_on() {
        let mut car = CarTyres {
            kept: vec![set(1, 9.0), set(2, 1.0), set(1, 4.0)],
            ..Default::default()
        };
        assert_eq!(car.take_used(1), Some(set(1, 4.0)));
        assert_eq!(car.take_used(1), Some(set(1, 9.0)));
        // None of that compound left: the least worn of any.
        assert_eq!(car.take_used(1), Some(set(2, 1.0)));
        assert_eq!(car.take_used(1), None);
    }
}
