//! What kind of barrier belongs at a given point on the circuit.
//!
//! Until now nothing decided this. The straight pass hard-coded
//! `armco_generic` and the corner pass copied whichever asset the old
//! procedural enrichment happened to leave lying nearby, which at the Red
//! Bull Ring meant `tire_wall_generic` everywhere and nothing else. The
//! kit has had `tecpro_2m`, `concrete_4m`, `concrete_4m_rail`,
//! `armco_4m_fence`, `armco_end`, `tires_corner` and `concrete_end` built
//! and documented for months, and no rule ever emitted one of them.
//!
//! There are two sources for the answer and they are used in that order.
//!
//! The first is the circuit itself. A layout dossier now carries the real
//! barrier lines out of OpenStreetMap — the Red Bull Ring has 25 runs
//! mapped `barrier=tyres` and 51 mapped `barrier=wall` — so where a cell
//! sits on top of one of those, the map decides and nothing here guesses.
//!
//! The second is the geometry, for the great majority of cells the map
//! says nothing about. That rule set is the one a circuit designer works
//! to: an impact at a shallow angle is caught by a rail that guides the
//! car along it, an impact square-on needs something that absorbs, and
//! what decides which you get is how much room there is between the road
//! and the barrier. So a corner with the run-off to take a car gets
//! armco; a corner without it gets Tecpro, which is what modern circuits
//! fit where a tyre wall used to go; and a tyre wall is kept for the
//! tightest corners, where a car arrives most nearly head-on.
//!
//! Everything here is pure and decided per 4 m cell, so it is testable on
//! its own and gives the same answer every run.

use crate::ats::PropKind;

/// What to put in one cell.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BarrierKind {
    /// Steel rail. The default, and what a straight wants.
    Armco,
    /// Steel rail with a debris fence on top, wherever there are people
    /// behind it.
    ArmcoFence,
    /// Plastic energy-absorbing blocks: a corner whose run-off is too
    /// short to slow a car before it arrives.
    Tecpro,
    /// Stacked tyres: the tightest corners, and wherever the map says so.
    Tyres,
    /// Concrete wall, with a rail on top where there are people behind.
    Concrete,
}

impl BarrierKind {
    /// The kit asset for a run of this kind, and the prop kind it is laid
    /// as. Tyres and Tecpro are both `TireWall` to the sim: the exporter
    /// maps either to the absorbing wall material.
    pub fn asset(self) -> (PropKind, &'static str) {
        match self {
            BarrierKind::Armco => (PropKind::Barrier, "armco_4m"),
            BarrierKind::ArmcoFence => (PropKind::Barrier, "armco_4m_fence"),
            BarrierKind::Tecpro => (PropKind::Barrier, "tecpro_2m"),
            BarrierKind::Tyres => (PropKind::TireWall, "tires_4m"),
            BarrierKind::Concrete => (PropKind::Barrier, "concrete_4m_rail"),
        }
    }

    /// The piece that closes the end of a run, if the kit has one.
    pub fn end_cap(self) -> Option<(PropKind, &'static str)> {
        match self {
            BarrierKind::Armco | BarrierKind::ArmcoFence => Some((PropKind::Barrier, "armco_end")),
            BarrierKind::Tecpro => Some((PropKind::Barrier, "tecpro_corner")),
            BarrierKind::Tyres => Some((PropKind::TireWall, "tires_corner")),
            BarrierKind::Concrete => Some((PropKind::Barrier, "concrete_end")),
        }
    }

    /// How long one module is, in metres. A Tecpro block is half the
    /// length of everything else, so a run of them needs twice as many.
    pub fn module_m(self) -> f32 {
        match self {
            BarrierKind::Tecpro => 2.0,
            _ => 4.0,
        }
    }
}

/// Every kind the decision can produce.
pub const ALL: [BarrierKind; 5] = [
    BarrierKind::Armco,
    BarrierKind::ArmcoFence,
    BarrierKind::Tecpro,
    BarrierKind::Tyres,
    BarrierKind::Concrete,
];

/// True for an asset the barrier pass lays, module or end cap. The pass
/// owns these: it deletes and re-lays them on every run, so the groomer
/// has to recognise its own output to adopt it back unchanged.
pub fn is_barrier_asset(asset: &str) -> bool {
    crate::circuit_style::VANGRAIL_ASSETS.contains(&asset)
        || ALL.iter().any(|kind| {
        let (_, module) = kind.asset();
        let cap = kind.end_cap().map(|(_, cap)| cap);
        module == asset || cap == Some(asset)
    })
}

/// What the decision knows about one cell.
#[derive(Debug, Clone, Copy)]
pub struct Cell {
    /// Tightest radius of the course within the corner window, metres.
    /// `f32::MAX` on a straight.
    pub corner_radius_m: f32,
    /// How far the prepared run-off reaches past the road edge here.
    pub runoff_m: f32,
    /// Distance to the nearest grandstand, spectator path or building,
    /// metres. `f32::MAX` when there is nothing behind the barrier.
    pub spectators_m: f32,
    /// Distance to the nearest mapped barrier run and what the map calls
    /// it, when one is close enough to be this cell's.
    pub mapped: Option<(f32, MappedKind)>,
    /// True on the pit straight's pit side and through pit entry/exit,
    /// where a concrete wall is what separates the two roads.
    pub beside_pit_lane: bool,
    /// Whether this side is the outside of the bend here.
    ///
    /// This is the signal that makes the mix look like a real circuit
    /// rather than a uniform ring of the same thing. A car that loses the
    /// road in a corner goes off on the outside; the inside barrier is
    /// there to stop a spin or a cut, and is armco almost everywhere.
    /// Without it the first version of this rule put Tecpro on both sides
    /// of every bend at the Red Bull Ring, 666 modules against 84 of
    /// armco, where the real circuit is overwhelmingly armco.
    pub outside_of_bend: bool,
}

/// What OpenStreetMap calls a barrier run, reduced to the kinds that mean
/// something here.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MappedKind {
    Tyres,
    Wall,
    Fence,
}

impl MappedKind {
    pub fn from_dossier(kind: &str) -> Option<Self> {
        match kind {
            "tyres" => Some(MappedKind::Tyres),
            "wall" => Some(MappedKind::Wall),
            "guard_rail" => None, // armco is already the default
            "fence" | "hedge" => Some(MappedKind::Fence),
            _ => None,
        }
    }
}

/// How close a mapped tyre wall has to be before it is taken to be this
/// cell's. The dossier's lines are traced to a couple of metres and the
/// generated line stands at the run-off's outer edge, so the two do not
/// coincide exactly; beyond this they are different barriers.
pub const MAPPED_MATCH_M: f32 = 12.0;

/// The same for a mapped wall.
///
/// `barrier=tyres` near a circuit is always the circuit's own, so it is
/// taken at face value. `barrier=wall` is not: some of it is the circuit
/// boundary, some is a retaining wall or a paddock enclosure, and a good
/// deal of it is how a mapper chose to trace the armco. The Red Bull Ring
/// has 7.3 km of it against a 4.3 km lap, and believing it put a concrete
/// wall round a third of the circuit — the Red Bull Ring has none.
///
/// So a mapped wall is treated as evidence that *a* barrier belongs
/// there, which the geometry rule already knows, and it only decides the
/// kind where it also sits hard against the road. That is the case
/// concrete is actually for: a street circuit with no run-off at all.
pub const MAPPED_WALL_MATCH_M: f32 = 5.0;

/// Run-off under this, beside a mapped wall, means the wall is hard
/// against the road: a street circuit, and concrete.
pub const WALL_AGAINST_ROAD_M: f32 = 4.0;

/// Run-off at or beyond this is enough to take the speed out of a car
/// before it reaches the barrier, so a rail will do.
pub const AMPLE_RUNOFF_M: f32 = 30.0;

/// Below this radius a car arrives at the barrier closer to head-on than
/// alongside, which is the case tyres are for.
pub const HEAD_ON_RADIUS_M: f32 = 80.0;

/// Run-off under this, on the outside of a corner, is too little to slow
/// a car before it arrives.
pub const SHORT_RUNOFF_M: f32 = 15.0;

/// A barrier with people this close behind it carries a debris fence.
pub const SPECTATOR_RANGE_M: f32 = 14.0;

/// Radius above which the corner is fast enough, and the angle shallow
/// enough, that a rail does the job. Real circuits fit their absorbing
/// barriers at the slow corners and leave the sweepers to armco.
pub const STRAIGHT_RADIUS_M: f32 = 150.0;

/// Decide one cell.
pub fn decide(cell: &Cell) -> BarrierKind {
    let spectators = cell.spectators_m <= SPECTATOR_RANGE_M;
    let rail = if spectators {
        BarrierKind::ArmcoFence
    } else {
        BarrierKind::Armco
    };

    // The map wins where it has something to say: these are the real
    // barriers, surveyed, not a guess from the shape of the road.
    if let Some((distance, mapped)) = cell.mapped {
        match mapped {
            MappedKind::Tyres if distance <= MAPPED_MATCH_M => return BarrierKind::Tyres,
            MappedKind::Wall
                if distance <= MAPPED_WALL_MATCH_M && cell.runoff_m <= WALL_AGAINST_ROAD_M =>
            {
                return BarrierKind::Concrete
            }
            // A mapped fence is a spectator fence, not a barrier that
            // stops a car; it means people, so the rail gets a fence on
            // top rather than becoming one.
            MappedKind::Fence if distance <= MAPPED_MATCH_M => return BarrierKind::ArmcoFence,
            _ => {}
        }
    }

    // Concrete separates the two roads down the pit straight.
    if cell.beside_pit_lane {
        return BarrierKind::Concrete;
    }

    // A car leaves the road on the outside of a bend. Everywhere else —
    // the straights, and the inside of every corner — a rail is both what
    // is needed and what circuits actually fit.
    if !cell.outside_of_bend || cell.corner_radius_m >= STRAIGHT_RADIUS_M {
        return rail;
    }

    // The outside of a real corner. What matters now is whether there is
    // room to slow down before the barrier.
    if cell.runoff_m >= AMPLE_RUNOFF_M {
        return rail;
    }
    if cell.corner_radius_m <= HEAD_ON_RADIUS_M && cell.runoff_m < SHORT_RUNOFF_M {
        // Slow and no room: a car reaches this nearly square-on.
        return BarrierKind::Tyres;
    }
    BarrierKind::Tecpro
}

#[cfg(test)]
mod tests {
    use super::*;

    fn cell() -> Cell {
        Cell {
            corner_radius_m: f32::MAX,
            runoff_m: 12.0,
            spectators_m: f32::MAX,
            mapped: None,
            beside_pit_lane: false,
            outside_of_bend: true,
        }
    }

    #[test]
    fn a_straight_gets_a_rail() {
        assert_eq!(decide(&cell()), BarrierKind::Armco);
    }

    #[test]
    fn people_behind_it_put_a_fence_on_the_rail() {
        let c = Cell {
            spectators_m: 8.0,
            ..cell()
        };
        assert_eq!(decide(&c), BarrierKind::ArmcoFence);
    }

    #[test]
    fn a_corner_with_room_still_gets_a_rail() {
        let c = Cell {
            corner_radius_m: 60.0,
            runoff_m: 45.0,
            ..cell()
        };
        assert_eq!(decide(&c), BarrierKind::Armco);
    }

    #[test]
    fn the_inside_of_a_corner_gets_a_rail() {
        // Cars go off on the outside. Putting an absorbing barrier on
        // both sides of every bend is what made the first version of
        // this rule produce eight Tecpro modules for every one of armco.
        let c = Cell {
            corner_radius_m: 40.0,
            runoff_m: 6.0,
            outside_of_bend: false,
            ..cell()
        };
        assert_eq!(decide(&c), BarrierKind::Armco);
    }

    #[test]
    fn a_fast_sweeper_gets_a_rail_even_on_the_outside() {
        let c = Cell {
            corner_radius_m: 260.0,
            runoff_m: 8.0,
            ..cell()
        };
        assert_eq!(decide(&c), BarrierKind::Armco);
    }

    #[test]
    fn a_fast_corner_without_room_gets_tecpro() {
        let c = Cell {
            corner_radius_m: 120.0,
            runoff_m: 8.0,
            ..cell()
        };
        assert_eq!(decide(&c), BarrierKind::Tecpro);
    }

    #[test]
    fn a_tight_corner_without_room_gets_tyres() {
        let c = Cell {
            corner_radius_m: 40.0,
            runoff_m: 6.0,
            ..cell()
        };
        assert_eq!(decide(&c), BarrierKind::Tyres);
    }

    #[test]
    fn the_map_wins_over_the_geometry() {
        // A straight that the map says is lined with tyres is lined with
        // tyres, whatever the shape of the road would have chosen.
        let c = Cell {
            mapped: Some((4.0, MappedKind::Tyres)),
            ..cell()
        };
        assert_eq!(decide(&c), BarrierKind::Tyres);

        // A wall hard against the road is a street circuit: concrete.
        let c = Cell {
            mapped: Some((4.0, MappedKind::Wall)),
            runoff_m: 2.0,
            ..cell()
        };
        assert_eq!(decide(&c), BarrierKind::Concrete);

        // The same wall with run-off behind it is the circuit boundary or
        // somebody's retaining wall, and decides nothing.
        let c = Cell {
            mapped: Some((4.0, MappedKind::Wall)),
            runoff_m: 12.0,
            ..cell()
        };
        assert_eq!(decide(&c), BarrierKind::Armco);

        // And one further off than a wall match decides nothing either.
        let c = Cell {
            mapped: Some((MAPPED_WALL_MATCH_M + 3.0, MappedKind::Wall)),
            runoff_m: 2.0,
            ..cell()
        };
        assert_eq!(decide(&c), BarrierKind::Armco);
    }

    #[test]
    fn a_mapped_barrier_too_far_away_is_somebody_elses() {
        let c = Cell {
            mapped: Some((MAPPED_MATCH_M + 5.0, MappedKind::Tyres)),
            ..cell()
        };
        assert_eq!(decide(&c), BarrierKind::Armco);
    }

    #[test]
    fn the_pit_straight_is_separated_by_concrete() {
        let c = Cell {
            beside_pit_lane: true,
            ..cell()
        };
        assert_eq!(decide(&c), BarrierKind::Concrete);
    }

    #[test]
    fn every_kind_has_an_asset_and_a_cap() {
        for kind in ALL {
            let (prop_kind, asset) = kind.asset();
            assert!(!asset.is_empty());
            assert!(matches!(prop_kind, PropKind::Barrier | PropKind::TireWall));
            let (cap_kind, cap) = kind.end_cap().expect("every kind closes its run");
            assert!(!cap.is_empty());
            assert!(matches!(cap_kind, PropKind::Barrier | PropKind::TireWall));
            assert!(kind.module_m() > 0.0);
        }
    }

    #[test]
    fn the_pass_recognises_everything_it_lays() {
        // The groomer adopts its own output back by asset name, so an
        // asset the pass can lay but not recognise is re-laid as a
        // duplicate on every run.
        for kind in ALL {
            let (_, module) = kind.asset();
            assert!(is_barrier_asset(module), "{module} not recognised");
            let (_, cap) = kind.end_cap().unwrap();
            assert!(is_barrier_asset(cap), "{cap} not recognised");
        }
        assert!(!is_barrier_asset("broadleaf_m"));
        assert!(!is_barrier_asset("hoarding_3m"));
    }

    #[test]
    fn guard_rail_on_the_map_is_the_default_not_an_override() {
        // OSM tags plain armco as `guard_rail`; that is what the geometry
        // rule already produces, so it must not short-circuit the richer
        // decision (a guard rail mapped through a spectator area still
        // wants its debris fence).
        assert_eq!(MappedKind::from_dossier("guard_rail"), None);
        assert_eq!(MappedKind::from_dossier("tyres"), Some(MappedKind::Tyres));
        assert_eq!(MappedKind::from_dossier("wall"), Some(MappedKind::Wall));
    }
}
