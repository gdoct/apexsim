//! The road's cross-section beyond the centerline's banking: a drainage
//! crown and a field of small bumps.
//!
//! The centerline gives the road a height and a banking, a plane sheared
//! across its width. Real tarmac is not that plane: a road on the flat falls
//! away from its middle to both edges so rain runs off (the crown), and it
//! rises and dips by millimetres along its length (laying seams, settling).
//! [`height_offset`] is how far the surface stands above that plane at a
//! point, the same for the content's road mesh (`ue_export`, through
//! `track_path::offset_point`) and the simulation's centerline backend
//! (`physics::surface_elevation`), so the two backends stand on one road.
//!
//! Past the road's edge the offset holds the edge's value: a curb, a verge
//! or a wall foot sits on the road edge as built. A banked road drains to
//! its low side and has no crown ([`crown_share`]). The bumps are a field
//! over the world's plan, not over the lap: the two sides measure a station
//! differently (the simulation along its 1 m points in 3D, the content
//! along its 2 m samples in plan), but agree on where a point is. An
//! imported circuit's road is its importer's, measured, and gets none of
//! this ([`crate::track::TrackFile::has_road_profile`]).
//!
//! Changing anything here moves every track's baked road and the AI survey:
//! re-export the tracks and judge the change with the survey.

/// Cross fall each side of the crown, as a grade (2%).
pub const CROWN_GRADE: f32 = 0.02;
/// Half the width of the crown's rounded top, metres: within it the
/// surface is a parabola meeting the cross falls with their own slope.
pub const CROWN_ROUND_M: f32 = 1.0;
/// Banking from which the crown starts to give way to a single cross fall,
/// rad (about 1.1 degrees)...
pub const CROWN_FADE_FROM_RAD: f32 = 0.02;
/// ...and by which it is gone (about 2.9 degrees).
pub const CROWN_FADE_TO_RAD: f32 = 0.05;

/// The bumps: plane waves over the plan, (wavelength m, amplitude m,
/// direction rad, phase rad). Incommensurate wavelengths in scattered
/// directions, so a road running any way crosses an irregular field; every
/// wavelength at least five metres, so the road mesh (a section a metre,
/// columns a metre) draws what the simulation drives. Their sum stays
/// within [`BUMP_MAX_M`].
const BUMPS: [(f32, f32, f32, f32); 6] = [
    (5.3, 0.0010, 0.3, 0.4),
    (7.9, 0.0012, 2.2, 2.9),
    (12.7, 0.0018, 1.1, 2.1),
    (19.3, 0.0022, 2.8, 5.0),
    (31.1, 0.0028, 0.7, 4.4),
    (57.7, 0.0030, 1.9, 1.3),
];

/// The most the bumps lift or drop the surface, metres.
pub const BUMP_MAX_M: f32 = 0.012;

/// The most the crown drops a road edge below the sheared plane, metres,
/// for a road `half_width_m` wide on that side.
pub fn crown_max_drop(half_width_m: f32) -> f32 {
    crown_drop(half_width_m)
}

/// How much of the crown a road banked `bank_rad` keeps: all of it on the
/// flat, none from [`CROWN_FADE_TO_RAD`].
pub fn crown_share(bank_rad: f32) -> f32 {
    let t = (bank_rad.abs() - CROWN_FADE_FROM_RAD) / (CROWN_FADE_TO_RAD - CROWN_FADE_FROM_RAD);
    1.0 - t.clamp(0.0, 1.0)
}

/// The full crown's drop below the middle at `|lat|` metres out.
fn crown_drop(lat_abs_m: f32) -> f32 {
    let x = lat_abs_m.abs();
    if x <= CROWN_ROUND_M {
        CROWN_GRADE * x * x / (2.0 * CROWN_ROUND_M)
    } else {
        CROWN_GRADE * (x - CROWN_ROUND_M / 2.0)
    }
}

/// The bumps' height at a point of the plan, metres.
pub fn bumps(x: f32, y: f32) -> f32 {
    use std::f32::consts::TAU;
    BUMPS
        .iter()
        .map(|&(wavelength, amp, dir, phase)| {
            let (sin_d, cos_d) = dir.sin_cos();
            amp * (TAU * (x * cos_d + y * sin_d) / wavelength + phase).sin()
        })
        .sum()
}

/// How far the road's surface stands above the centerline's sheared plane
/// at `lat_m` (positive left) across a section whose centerline point is
/// `(x, y)` heading `heading_rad` (CCW from +X), on a road `width_left_m` /
/// `width_right_m` wide each side and banked `bank_rad`. Past an edge, the
/// edge's value: the crown's and the bumps' at the edge point.
pub fn height_offset(
    x: f32,
    y: f32,
    heading_rad: f32,
    lat_m: f32,
    width_left_m: f32,
    width_right_m: f32,
    bank_rad: f32,
) -> f32 {
    let lat = lat_m.clamp(-width_right_m.max(0.0), width_left_m.max(0.0));
    let (sin_h, cos_h) = heading_rad.sin_cos();
    bumps(x - sin_h * lat, y + cos_h * lat) - crown_share(bank_rad) * crown_drop(lat)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The offset less the bumps, at a section on the x axis heading +X.
    fn crown_at(lat: f32, left: f32, right: f32, bank: f32) -> f32 {
        height_offset(0.0, 0.0, 0.0, lat, left, right, bank) - bumps(0.0, lat.clamp(-right, left))
    }

    #[test]
    fn the_crown_falls_two_percent_to_each_edge() {
        assert!(crown_at(0.0, 6.0, 6.0, 0.0).abs() < 1e-6);
        let edge = crown_at(6.0, 6.0, 6.0, 0.0);
        assert!((edge + 0.02 * 5.5).abs() < 1e-4, "{edge}");
        assert!((crown_at(-6.0, 6.0, 6.0, 0.0) - edge).abs() < 1e-4);
    }

    #[test]
    fn the_crown_is_smooth_where_the_rounding_meets_the_fall() {
        let below = crown_drop(CROWN_ROUND_M - 1e-3);
        let above = crown_drop(CROWN_ROUND_M + 1e-3);
        let slope = (above - below) / 2e-3;
        assert!((slope - CROWN_GRADE).abs() < 1e-3, "{slope}");
    }

    #[test]
    fn a_banked_road_has_no_crown() {
        assert_eq!(crown_share(0.0), 1.0);
        assert_eq!(crown_share(-0.08), 0.0);
        assert!(crown_at(5.0, 6.0, 6.0, 0.08).abs() < 1e-6);
    }

    #[test]
    fn past_the_edge_the_edge_holds() {
        let at = |lat| height_offset(3.0, -2.0, 0.4, lat, 6.0, 4.0, 0.0);
        assert_eq!(at(9.0), at(6.0));
        assert_eq!(at(-7.5), at(-4.0));
    }

    #[test]
    fn the_bumps_are_where_the_point_is_whatever_the_section() {
        // The same point reached from two centerline points and headings.
        let a = height_offset(10.0, 0.0, 0.0, 3.0, 6.0, 6.0, 0.08);
        let b = height_offset(13.0, 3.0, std::f32::consts::FRAC_PI_2, 3.0, 6.0, 6.0, 0.08);
        assert!((a - bumps(10.0, 3.0)).abs() < 1e-6);
        assert!((b - bumps(10.0, 3.0)).abs() < 1e-6);
    }

    #[test]
    fn the_bumps_stay_within_their_bound() {
        let mut worst = 0.0f32;
        for i in 0..200 {
            for j in 0..200 {
                worst = worst.max(bumps(i as f32 * 1.37 - 150.0, j as f32 * 2.11 + 900.0).abs());
            }
        }
        assert!(worst <= BUMP_MAX_M, "{worst}");
        assert!(worst > 0.005, "{worst}");
    }
}
