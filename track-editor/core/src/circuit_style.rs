//! What sets one circuit's generated furniture apart from the others.
//!
//! The groomer's rules were written for modern Grand Prix circuits: armco
//! 7 m (straights) to 14 m (corners) off the road with run-off in front of
//! it, a tree belt starting 22 m out, braking boards before every corner
//! and hoardings on the rails. The Nordschleife is none of that: a public
//! road's German guard rail ("Schutzplanke", the Dutch *vangrail*) three
//! or four metres off the tarmac, the forest right behind it, yellow-black
//! kilometre boards and red-white chevrons instead of brands and distance
//! boards. A [`CircuitStyle`] carries those choices, picked by the scene's
//! source track stem (the way `dress::RUNOFF_PAINT` picks a paint style);
//! every other circuit gets [`CircuitStyle::DEFAULT`], which is exactly the
//! rules as they were.

use crate::ats::AtsScene;

/// Which steel rail the barrier pass lays where the decision says "armco".
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Rail {
    /// The kit's UK-style three-rail `armco_4m` (and `_fence`, `armco_end`).
    Armco,
    /// German guard rail: `vangrail_4m` (double beam) in corners,
    /// `vangrail_4m_triple` on the fast stretches, `vangrail_4m_fence`
    /// where people stand behind it, `vangrail_end` closing a run.
    Vangrail,
}

/// Every asset the vangrail style can lay, for the barrier pass's
/// ownership test.
pub const VANGRAIL_ASSETS: [&str; 4] = [
    "vangrail_4m",
    "vangrail_4m_triple",
    "vangrail_4m_fence",
    "vangrail_end",
];

/// A straighter stretch than this (tightest radius, metres) gets the
/// triple-height rail in the vangrail style.
pub const VANGRAIL_TRIPLE_RADIUS_M: f32 = 400.0;

impl Rail {
    /// The asset the barrier pass lays for a kit key, on a stretch whose
    /// tightest radius is `radius_m`. Non-rail keys pass through.
    pub fn asset(self, kit_asset: &'static str, radius_m: f32) -> &'static str {
        match (self, kit_asset) {
            (Rail::Armco, _) => kit_asset,
            (Rail::Vangrail, "armco_4m") if radius_m >= VANGRAIL_TRIPLE_RADIUS_M => {
                "vangrail_4m_triple"
            }
            (Rail::Vangrail, "armco_4m") => "vangrail_4m",
            (Rail::Vangrail, "armco_4m_fence") => "vangrail_4m_fence",
            (Rail::Vangrail, "armco_end") => "vangrail_end",
            (Rail::Vangrail, other) => other,
        }
    }
}

/// Where and how thickly the tree belts grow.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct TreeBelt {
    /// Nearest a belt tree stands to the road edge, metres.
    pub near_m: f32,
    /// Farthest, metres; the impostor forest takes over beyond it.
    pub far_m: f32,
    /// Trees per 12 m cell and side, before the density multiplier.
    pub min_per_cell: u32,
    pub max_per_cell: u32,
    /// Share of cells left empty, for clearings.
    pub empty_share: f32,
}

/// Which roadside signs the groomer lays.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RoadSigns {
    /// Braking boards before the corners (the kit's `braking_marker`).
    BrakingBoards,
    /// The Nordschleife's: red-white chevron boards round the outside of
    /// every tight corner, a kilometre board every kilometre, bend and
    /// danger warnings before the tightest ones and the Touristenfahrten
    /// "overtake on the left" boards. No braking boards.
    German,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct CircuitStyle {
    pub rail: Rail,
    /// Closest a straight's barrier stands to the road edge, metres.
    pub straight_barrier_min_m: f32,
    /// Closest a corner's barrier stands to the road edge, metres.
    pub corner_barrier_min_m: f32,
    /// Advertising hoardings on the rails.
    pub hoardings: bool,
    pub trees: TreeBelt,
    pub signs: RoadSigns,
}

impl CircuitStyle {
    /// The rules every circuit was groomed by before styles existed.
    pub const DEFAULT: CircuitStyle = CircuitStyle {
        rail: Rail::Armco,
        straight_barrier_min_m: 7.0,
        corner_barrier_min_m: 14.0,
        hoardings: true,
        trees: TreeBelt {
            near_m: 22.0,
            far_m: 90.0,
            min_per_cell: 3,
            max_per_cell: 6,
            empty_share: 0.10,
        },
        signs: RoadSigns::BrakingBoards,
    };

    /// The Nordschleife: guard rail close to a narrow road, forest right
    /// behind it, German signs, no hoardings.
    pub const NORDSCHLEIFE: CircuitStyle = CircuitStyle {
        rail: Rail::Vangrail,
        straight_barrier_min_m: 3.0,
        corner_barrier_min_m: 4.5,
        hoardings: false,
        trees: TreeBelt {
            near_m: 8.5,
            far_m: 110.0,
            min_per_cell: 6,
            max_per_cell: 11,
            empty_share: 0.0,
        },
        signs: RoadSigns::German,
    };

    /// The style of the circuit a scene decorates, by its source track's
    /// stem.
    pub fn for_scene(scene: &AtsScene) -> CircuitStyle {
        let stem = scene
            .source_track
            .rsplit_once('.')
            .map_or(scene.source_track.as_str(), |(stem, _)| stem);
        Self::for_stem(stem)
    }

    pub fn for_stem(stem: &str) -> CircuitStyle {
        match stem {
            "Nordschleife" => Self::NORDSCHLEIFE,
            _ => Self::DEFAULT,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_other_circuit_keeps_the_old_rules() {
        assert_eq!(CircuitStyle::for_stem("Monza"), CircuitStyle::DEFAULT);
        assert_eq!(
            CircuitStyle::for_stem("Nordschleife"),
            CircuitStyle::NORDSCHLEIFE
        );
    }

    #[test]
    fn the_vangrail_style_swaps_only_the_rails() {
        let v = Rail::Vangrail;
        assert_eq!(v.asset("armco_4m", 100.0), "vangrail_4m");
        assert_eq!(v.asset("armco_4m", f32::MAX), "vangrail_4m_triple");
        assert_eq!(v.asset("armco_4m_fence", 100.0), "vangrail_4m_fence");
        assert_eq!(v.asset("armco_end", 100.0), "vangrail_end");
        assert_eq!(v.asset("tecpro_2m", 100.0), "tecpro_2m");
        assert_eq!(Rail::Armco.asset("armco_4m", 100.0), "armco_4m");
        for a in VANGRAIL_ASSETS {
            assert!(crate::props::resolve(crate::ats::PropKind::Barrier, a).is_some(), "{a} not in the kit");
        }
    }
}
