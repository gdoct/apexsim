//! How ribbon-shaped elements sit on the ground: the lifts that stack them
//! without z-fighting, the lateral columns a ground band is cut into, the
//! height a band vertex takes and each surface kind's colour.
//!
//! The Unreal bake ([`crate::ue_export`]) and the editor viewport's preview
//! meshes both build from these, so the two agree on where a band is even
//! though only the bake's geometry is lit and shipped.

use crate::ats::{Surface, SurfaceKind};
use crate::terrain::TerrainHeightfield;
use crate::track_path::PathSample;

/// How far strips float above the surface they decorate, to avoid
/// z-fighting: ground < ground surfaces < track < pit lane < curbs < markings.
pub const CURB_LIFT_M: f32 = 0.02;
pub const MARKING_LIFT_M: f32 = 0.04;
/// Ground patches sit on the ground of [`TerrainHeightfield::ground_height_at`]
/// — which is itself [`crate::terrain::VERGE_DROP_M`] under the road, so the
/// ribbon always wins where they meet. The base lift clears the ground
/// mesh's own facets (a 4 m facet cuts a few centimetres inside a curved
/// verge profile), and each surface kind gets its own sliver of height so a
/// gravel trap laid over the grass band doesn't z-fight with it.
const SURFACE_BASE_LIFT_M: f32 = 0.03;
const SURFACE_LAYER_STEP_M: f32 = 0.008;

pub fn surface_lift(kind: SurfaceKind) -> f32 {
    SURFACE_BASE_LIFT_M + kind.layer() as f32 * SURFACE_LAYER_STEP_M
}

/// Height of a ground-band vertex before its layer lift: the ground that
/// is actually there — [`TerrainHeightfield::ground_height_at`], the verge
/// beside every road blending into the terrain further out. Without a
/// field (degenerate paths only) the vertex keeps its road-following
/// height. `sample` and `lat_m` are kept for callers that already resolved
/// the vertex against the track; the height is a function of `pos` alone.
pub fn surface_height(
    terrain: Option<&TerrainHeightfield>,
    _sample: &PathSample,
    _lat_m: f32,
    pos: (f32, f32, f32),
) -> f32 {
    match terrain {
        Some(field) => field.ground_height_at(pos.0, pos.1),
        None => pos.2,
    }
}

/// Lateral spacing of the extra vertex columns inside a ground band, as a
/// schedule of `(up to this far out, column spacing)`. The ground profile
/// bends most within the blend zone beside the road, so columns are dense
/// there and open out further away; a band left as one quad laterally
/// would just be a plane from its inner border to its outer one, sailing
/// over (or under) anything in between.
const SURFACE_COLUMN_SCHEDULE: [(f32, f32); 3] = [(12.0, 2.0), (40.0, 4.0), (f32::INFINITY, 10.0)];
/// Upper bound on those columns, for 130 m grass aprons.
pub const SURFACE_MAX_COLUMNS: usize = 40;

/// Lateral vertex columns of a surface, as fractions of its (widest)
/// width from the inner border (`0.0`) to the outer (`1.0`). Constant along
/// the band, as strip extrusion requires; on a tapering band the columns
/// simply bunch up at the narrow end.
pub fn surface_lateral_fractions(surface: &Surface) -> Vec<f32> {
    let width = surface
        .width_m
        .max(surface.end_width_m.unwrap_or(surface.width_m))
        .max(0.01);
    let mut offsets = vec![0.0f32];
    let mut at = 0.0f32;
    while at < width && offsets.len() < SURFACE_MAX_COLUMNS {
        let step = SURFACE_COLUMN_SCHEDULE
            .iter()
            .find(|(until, _)| at < *until)
            .map_or(10.0, |(_, step)| *step);
        at = (at + step).min(width);
        // Don't leave a sliver column at the outer border.
        if width - at < step * 0.25 {
            at = width;
        }
        offsets.push(at);
    }
    if *offsets.last().unwrap() < width {
        offsets.push(width);
    }
    offsets.iter().map(|o| o / width).collect()
}

/// Preview color per surface kind. Deliberately close to the palette
/// `track_path::surface_color` uses for the track's own surface types, so a
/// gravel trap reads the same as a gravel-surfaced track node.
pub fn surface_kind_color(kind: SurfaceKind) -> [f32; 4] {
    match kind {
        SurfaceKind::Grass => [0.16, 0.42, 0.14, 1.0],
        SurfaceKind::Gravel => [0.62, 0.55, 0.4, 1.0],
        SurfaceKind::AsphaltRunoff => [0.3, 0.3, 0.33, 1.0],
        SurfaceKind::Concrete => [0.55, 0.55, 0.55, 1.0],
        SurfaceKind::Sand => [0.76, 0.68, 0.42, 1.0],
        SurfaceKind::Astroturf => [0.1, 0.34, 0.12, 1.0],
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ats::Side;
    use crate::terrain;
    use crate::track_data::{TrackFile, TrackNode};
    use crate::track_path::{offset_point, CenterlinePath};

    fn node(x: f32, y: f32) -> TrackNode {
        TrackNode {
            x,
            y,
            z: 0.0,
            width: None,
            width_left: None,
            width_right: None,
            banking: None,
            friction: None,
            surface_type: None,
        }
    }

    fn loop_track() -> TrackFile {
        TrackFile {
            name: "Loop".to_string(),
            track_id: None,
            nodes: vec![
                node(0.0, 0.0),
                node(100.0, 0.0),
                node(100.0, 100.0),
                node(0.0, 100.0),
            ],
            checkpoints: vec![],
            spawn_points: vec![],
            default_width: 12.0,
            closed_loop: true,
            raceline: vec![],
            drs_zones: Vec::new(),
            metadata: None,
        }
    }

    #[test]
    fn surface_layers_stack_under_the_track_in_kind_order() {
        for kind in SurfaceKind::ALL {
            // Bands sit on the verge, which is `VERGE_DROP_M` under the
            // road edge: the lift must not bring them back up to it.
            assert!(
                surface_lift(kind) > 0.0 && surface_lift(kind) < terrain::VERGE_DROP_M,
                "{} must sit above the ground and under the road",
                kind.label()
            );
        }
        // Broad ground first, specific patches on top of it.
        assert!(surface_lift(SurfaceKind::Grass) < surface_lift(SurfaceKind::AsphaltRunoff));
        assert!(surface_lift(SurfaceKind::AsphaltRunoff) < surface_lift(SurfaceKind::Gravel));
        assert!(surface_lift(SurfaceKind::Gravel) < surface_lift(SurfaceKind::Astroturf));
    }

    /// The regression behind floating terrain: a wide band from a high
    /// section must come down to the terrain field at its far edge instead
    /// of hanging at its own station's height.
    #[test]
    fn wide_surface_bands_blend_down_to_the_terrain() {
        let mut track = loop_track();
        // Raise one straight 40 m above the rest.
        track.nodes[2].z = 40.0;
        track.nodes[3].z = 40.0;
        let path = CenterlinePath::from_track(&track).unwrap();
        let terrain = TerrainHeightfield::from_path(&path).unwrap();

        let sample = path.sample_at(path.total_length_m() * 0.55); // on the high side
        let near = surface_height(Some(&terrain), &sample, sample.width_left_m + 1.0, {
            let mut p = offset_point(&sample, sample.width_left_m + 1.0);
            p.2 = sample.pos.2;
            p
        });
        let far_lat = sample.width_left_m + 120.0;
        let far_pos = offset_point(&sample, far_lat);
        let far = surface_height(
            Some(&terrain),
            &sample,
            far_lat,
            (far_pos.0, far_pos.1, sample.pos.2),
        );

        // Within the shoulder the band hugs its own road edge (less the
        // verge drop). Far out it obeys the terrain, which 120 m from a
        // 40 m ridge is well below it.
        // (Loose: this synthetic loop climbs at 40 % through a corner, so
        // a centimetre of station error is a few centimetres of height.)
        assert!(
            (near - (sample.pos.2 - terrain::VERGE_DROP_M)).abs() < 0.1,
            "near {near} vs {}",
            sample.pos.2 - terrain::VERGE_DROP_M
        );
        assert!(
            far < sample.pos.2 - 1.0,
            "far edge {far} never came down from {}",
            sample.pos.2
        );
    }

    /// The regression behind buried roads: the blend alone leaves a band
    /// vertex 10–35 m out only *partially* lowered, so a band from a high
    /// section still hung meters over a lower road passing nearby. The
    /// ceiling clamp must bring it under that road.
    #[test]
    fn band_never_hangs_over_a_lower_parallel_road() {
        // Two long parallel legs 30 m apart, one 25 m above the other.
        let track = TrackFile {
            name: "TwoLevels".to_string(),
            track_id: None,
            nodes: vec![
                node(0.0, 0.0),
                node(500.0, 0.0),
                {
                    let mut n = node(500.0, 30.0);
                    n.z = 25.0;
                    n
                },
                {
                    let mut n = node(0.0, 30.0);
                    n.z = 25.0;
                    n
                },
            ],
            checkpoints: vec![],
            spawn_points: vec![],
            default_width: 10.0,
            closed_loop: true,
            raceline: vec![],
            drs_zones: Vec::new(),
            metadata: None,
        };
        let path = CenterlinePath::from_track(&track).unwrap();
        let terrain = TerrainHeightfield::from_path(&path).unwrap();

        // A cross-section on the high leg, mid-straight.
        let sample = *path
            .samples()
            .iter()
            .find(|s| s.pos.2 > 20.0 && (s.pos.0 - 250.0).abs() < 30.0)
            .expect("high leg sample");

        // A band vertex 25 m beyond the edge, on whichever side reaches
        // over the low leg at y = 0.
        let (lat, pos) = [25.0f32, -25.0f32]
            .map(|beyond| {
                let lat = beyond.signum() * (sample.width_left_m + beyond.abs());
                (lat, offset_point(&sample, lat))
            })
            .into_iter()
            .min_by(|a, b| a.1 .1.abs().partial_cmp(&b.1 .1.abs()).unwrap())
            .unwrap();
        assert!(pos.1.abs() < 15.0, "vertex not over the low leg: {pos:?}");

        let z = surface_height(Some(&terrain), &sample, lat, pos);
        assert!(
            z < 0.0,
            "band at z = {z} still hangs over the road at z = 0"
        );
    }

    #[test]
    fn surface_columns_are_dense_beside_the_road_and_reach_the_outer_border() {
        let surface = Surface {
            id: 1,
            kind: SurfaceKind::Grass,
            side: Side::Left,
            start_m: 0.0,
            end_m: 0.0,
            inner_m: 0.0,
            width_m: 130.0,
            end_width_m: None,
        };
        let f = surface_lateral_fractions(&surface);
        assert_eq!(f[0], 0.0);
        assert_eq!(*f.last().unwrap(), 1.0);
        assert!(f.windows(2).all(|w| w[1] > w[0]));
        // 2 m columns for the first 12 m…
        assert!((f[1] * 130.0 - 2.0).abs() < 1e-3);
        assert!((f[6] * 130.0 - 12.0).abs() < 1e-3);
        // …never coarser than 10 m, and bounded.
        assert!(f.windows(2).all(|w| (w[1] - w[0]) * 130.0 <= 10.0 + 1e-3));
        assert!(f.len() <= SURFACE_MAX_COLUMNS);

        let narrow = Surface {
            width_m: 1.5,
            ..surface
        };
        assert_eq!(surface_lateral_fractions(&narrow), vec![0.0, 1.0]);
    }
}
