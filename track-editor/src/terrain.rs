//! A coarse terrain heightfield derived from the track's own centerline.
//!
//! Tracks carry no terrain of their own — only the centerline has heights.
//! Ground bands used to be extruded sideways at their own station's height,
//! which reads fine on a flat circuit and falls apart the moment a track
//! folds back on itself with an elevation change: a 130 m grass band from a
//! high section would hang in mid-air above (or knife through) a lower
//! section 30 m away.
//!
//! The fix is a shared notion of "the ground here": every centerline sample
//! contributes its height to a regular grid via inverse-distance weighting,
//! so the terrain between two neighbouring track sections interpolates
//! smoothly between their elevations — and *at* any track section it agrees
//! with that section's own height. Ground bands blend from the road edge
//! into this field as they extend outward ([`blend_toward_terrain`]), and a
//! ground mesh built straight from the grid fills the world under
//! everything else.
//!
//! Building the field is deterministic: fixed iteration order, no wall
//! clock, plain `f32` arithmetic — repeated bakes stay byte-identical.

use crate::track_path::CenterlinePath;

/// Grid spacing of the sampled heightfield, meters. Coarse on purpose: this
/// is rolling ground, not road surface.
const CELL_M: f32 = 12.0;
/// Extra ground beyond the centerline's bounding box, meters. Must cover
/// the widest surface band content ships (130 m) with room to spare.
// Wide enough that, with height fog over it, the mesh edge sits in haze
// instead of cutting a visible line against the sky from driver height.
const MARGIN_M: f32 = 800.0;
/// Spacing between centerline samples used as height sources.
const SOURCE_SPACING_M: f32 = 15.0;
/// Softening added to the squared distance in the IDW weight, m². Keeps the
/// field smooth right at a source instead of pinching to a cone tip.
const IDW_SOFTENING_M2: f32 = 25.0;

/// The terrain must stay at least this far below any nearby road surface —
/// the road is a hard ceiling, never something the ground may cover.
const ROAD_CLEARANCE_M: f32 = 0.4;
/// Flat apron beyond the road edge that stays fully cleared, meters.
const APRON_M: f32 = 8.0;
/// How fast the ceiling rises back up beyond the apron (m per m). Hills
/// between two track sections at different heights become cuttings whose
/// walls climb at this grade, instead of blankets over the lower road.
const RISE_SLOPE: f32 = 0.15;

/// How far below every scene surface the ground mesh sits, meters.
pub const GROUND_LIFT_M: f32 = -0.25;

/// Lateral distance beyond the track edge where a ground band still follows
/// the road surface exactly, meters…
pub const BLEND_START_M: f32 = 6.0;
/// …and where it has fully blended into the terrain field.
pub const BLEND_END_M: f32 = 35.0;

/// How far above the carved terrain a ground band may reach. The terrain
/// is guaranteed to stay [`ROAD_CLEARANCE_M`] under every road, so a band
/// clamped to `terrain + BAND_CLEAR_M` can never bury one either — while a
/// band at its own road edge (terrain = road − 0.4 there) still reaches to
/// a few centimeters under its own road surface.
pub const BAND_CLEAR_M: f32 = 0.3;

/// Preview / material color for the ground itself: darker than any grass
/// band so authored surfaces still read on top of it.
pub const GROUND_COLOR: [f32; 4] = [0.11, 0.27, 0.10, 1.0];

pub struct TerrainHeightfield {
    /// Track-space position of grid vertex (0, 0).
    origin: (f32, f32),
    cell_m: f32,
    /// Grid vertex counts (not cell counts).
    cols: usize,
    rows: usize,
    /// Row-major heights, `rows * cols` entries.
    heights: Vec<f32>,
}

impl TerrainHeightfield {
    /// Build the field for a sampled centerline. Returns `None` for a
    /// degenerate path (no samples, or a bounding box of zero extent).
    pub fn from_path(path: &CenterlinePath) -> Option<Self> {
        let samples = path.samples();
        if samples.is_empty() {
            return None;
        }

        /// One centerline sample as the terrain sees it: an IDW height
        /// source, and a ceiling the ground may not rise through nearby.
        struct Source {
            x: f32,
            y: f32,
            z: f32,
            /// Ceiling right at the sample: the *low* road edge (banking can
            /// drop an edge well below the centerline) minus the clearance.
            ceiling_z: f32,
            /// Distance from the centerline out to which the ceiling stays
            /// flat: the road half-width plus the apron.
            reach_m: f32,
        }

        let (mut min_x, mut min_y) = (f32::MAX, f32::MAX);
        let (mut max_x, mut max_y) = (f32::MIN, f32::MIN);
        let mut sources: Vec<Source> = Vec::new();
        let mut next_station = 0.0f32;
        for s in samples {
            min_x = min_x.min(s.pos.0);
            min_y = min_y.min(s.pos.1);
            max_x = max_x.max(s.pos.0);
            max_y = max_y.max(s.pos.1);
            if s.station_m >= next_station {
                let half_width = s.width_left_m.max(s.width_right_m);
                let edge_drop = half_width * s.banking_rad.sin().abs();
                sources.push(Source {
                    x: s.pos.0,
                    y: s.pos.1,
                    z: s.pos.2,
                    ceiling_z: s.pos.2 - edge_drop - ROAD_CLEARANCE_M,
                    reach_m: half_width + APRON_M,
                });
                next_station = s.station_m + SOURCE_SPACING_M;
            }
        }
        if sources.is_empty() || !(min_x < max_x || min_y < max_y) {
            return None;
        }

        let origin = (min_x - MARGIN_M, min_y - MARGIN_M);
        let cols = (((max_x + MARGIN_M - origin.0) / CELL_M).ceil() as usize).max(1) + 1;
        let rows = (((max_y + MARGIN_M - origin.1) / CELL_M).ceil() as usize).max(1) + 1;

        let mut heights = Vec::with_capacity(rows * cols);
        for r in 0..rows {
            let y = origin.1 + r as f32 * CELL_M;
            for c in 0..cols {
                let x = origin.0 + c as f32 * CELL_M;
                let mut num = 0.0f32;
                let mut den = 0.0f32;
                let mut ceiling = f32::INFINITY;
                for s in &sources {
                    let d2 = (x - s.x).powi(2) + (y - s.y).powi(2);
                    let w = 1.0 / (d2 + IDW_SOFTENING_M2);
                    num += s.z * w;
                    den += w;
                    let allowance = (d2.sqrt() - s.reach_m).max(0.0) * RISE_SLOPE;
                    ceiling = ceiling.min(s.ceiling_z + allowance);
                }
                // The IDW average is the shape of the land; the ceiling is
                // the guarantee that no road is ever buried by it.
                heights.push((num / den).min(ceiling));
            }
        }

        Some(Self {
            origin,
            cell_m: CELL_M,
            cols,
            rows,
            heights,
        })
    }

    /// Terrain height at a track-space position, bilinear between grid
    /// vertices, clamped to the grid at its borders.
    pub fn height_at(&self, x: f32, y: f32) -> f32 {
        let fx = ((x - self.origin.0) / self.cell_m).clamp(0.0, (self.cols - 1) as f32);
        let fy = ((y - self.origin.1) / self.cell_m).clamp(0.0, (self.rows - 1) as f32);
        let c0 = fx.floor() as usize;
        let r0 = fy.floor() as usize;
        let c1 = (c0 + 1).min(self.cols - 1);
        let r1 = (r0 + 1).min(self.rows - 1);
        let tx = fx - c0 as f32;
        let ty = fy - r0 as f32;

        let h = |r: usize, c: usize| self.heights[r * self.cols + c];
        let top = h(r0, c0) + (h(r0, c1) - h(r0, c0)) * tx;
        let bottom = h(r1, c0) + (h(r1, c1) - h(r1, c0)) * tx;
        top + (bottom - top) * ty
    }

    /// Track-space position of grid vertex `(col, row)`.
    pub fn vertex(&self, col: usize, row: usize) -> (f32, f32, f32) {
        (
            self.origin.0 + col as f32 * self.cell_m,
            self.origin.1 + row as f32 * self.cell_m,
            self.heights[row * self.cols + col],
        )
    }

    /// Outward (up) surface normal at grid vertex `(col, row)`, track space,
    /// from central differences of the stored heights.
    pub fn normal(&self, col: usize, row: usize) -> (f32, f32, f32) {
        let h = |r: usize, c: usize| self.heights[r * self.cols + c];
        let (c0, c1) = (col.saturating_sub(1), (col + 1).min(self.cols - 1));
        let (r0, r1) = (row.saturating_sub(1), (row + 1).min(self.rows - 1));
        let dx = (h(row, c1) - h(row, c0)) / ((c1 - c0).max(1) as f32 * self.cell_m);
        let dy = (h(r1, col) - h(r0, col)) / ((r1 - r0).max(1) as f32 * self.cell_m);
        let len = (dx * dx + dy * dy + 1.0).sqrt();
        (-dx / len, -dy / len, 1.0 / len)
    }

    pub fn cols(&self) -> usize {
        self.cols
    }

    pub fn rows(&self) -> usize {
        self.rows
    }

    pub fn cell_m(&self) -> f32 {
        self.cell_m
    }
}

/// Blend a ground-band vertex height from the road edge into the terrain.
///
/// `edge_z` is the height the band would have had following the road
/// (centerline + banking shear), `terrain_z` the field's height at the
/// vertex's XY, and `beyond_edge_m` how far the vertex sits outside the
/// track edge. Inside [`BLEND_START_M`] the band hugs the road exactly so
/// curbs and verges stay seated; past [`BLEND_END_M`] it lies on the
/// terrain, whatever its own station's height was.
pub fn blend_toward_terrain(edge_z: f32, terrain_z: f32, beyond_edge_m: f32) -> f32 {
    let t = ((beyond_edge_m - BLEND_START_M) / (BLEND_END_M - BLEND_START_M)).clamp(0.0, 1.0);
    // Smoothstep, so the band leaves the road edge with zero slope.
    let t = t * t * (3.0 - 2.0 * t);
    edge_z + (terrain_z - edge_z) * t
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::track_data::{TrackFile, TrackNode};

    fn node(x: f32, y: f32, z: f32) -> TrackNode {
        TrackNode {
            x,
            y,
            z,
            width: None,
            width_left: None,
            width_right: None,
            banking: None,
            friction: None,
            surface_type: None,
        }
    }

    /// A loop with a 30 m hill on its far side.
    fn hilly_loop() -> TrackFile {
        TrackFile {
            name: "Hill".to_string(),
            track_id: None,
            nodes: vec![
                node(0.0, 0.0, 0.0),
                node(400.0, 0.0, 0.0),
                node(400.0, 300.0, 30.0),
                node(0.0, 300.0, 30.0),
            ],
            checkpoints: vec![],
            spawn_points: vec![],
            default_width: 10.0,
            closed_loop: true,
            raceline: vec![],
            metadata: None,
        }
    }

    fn field() -> TerrainHeightfield {
        let path = CenterlinePath::from_track(&hilly_loop()).unwrap();
        TerrainHeightfield::from_path(&path).unwrap()
    }

    #[test]
    fn terrain_agrees_with_the_centerline_at_the_centerline() {
        let f = field();
        // On the low straight the field must be near 0, on the high straight
        // near 30 — not some global average of the two.
        assert!(
            f.height_at(200.0, 0.0).abs() < 3.0,
            "{}",
            f.height_at(200.0, 0.0)
        );
        assert!(
            (f.height_at(200.0, 300.0) - 30.0).abs() < 3.0,
            "{}",
            f.height_at(200.0, 300.0)
        );
    }

    #[test]
    fn terrain_between_sections_interpolates() {
        let f = field();
        let mid = f.height_at(200.0, 150.0);
        assert!(mid > 5.0 && mid < 25.0, "midpoint height {mid}");
    }

    #[test]
    fn heights_and_normals_are_finite_everywhere() {
        let f = field();
        for r in 0..f.rows() {
            for c in 0..f.cols() {
                let (x, y, z) = f.vertex(c, r);
                assert!(x.is_finite() && y.is_finite() && z.is_finite());
                let n = f.normal(c, r);
                assert!(n.0.is_finite() && n.1.is_finite() && n.2.is_finite());
                assert!(n.2 > 0.0, "terrain normal must point up");
            }
        }
    }

    /// The regression behind buried roads: two sections running close
    /// together at very different heights. The averaged field would sit
    /// meters above the lower road; the ceiling has to keep the terrain
    /// under *both* roads.
    #[test]
    fn terrain_never_buries_a_nearby_lower_road() {
        let track = TrackFile {
            name: "TwoLevels".to_string(),
            track_id: None,
            nodes: vec![
                node(0.0, 0.0, 0.0),
                node(500.0, 0.0, 0.0),
                node(500.0, 30.0, 25.0),
                node(0.0, 30.0, 25.0),
            ],
            checkpoints: vec![],
            spawn_points: vec![],
            default_width: 10.0,
            closed_loop: true,
            raceline: vec![],
            metadata: None,
        };
        let path = CenterlinePath::from_track(&track).unwrap();
        let f = TerrainHeightfield::from_path(&path).unwrap();

        for x in [100.0, 250.0, 400.0] {
            // Under the low road: strictly below its surface at z = 0…
            assert!(
                f.height_at(x, 0.0) < -0.2,
                "terrain {} at ({x}, 0) buries the low road",
                f.height_at(x, 0.0)
            );
            // …and under the high road likewise, below z = 25.
            assert!(
                f.height_at(x, 30.0) < 24.8,
                "terrain {} at ({x}, 30) buries the high road",
                f.height_at(x, 30.0)
            );
        }
    }

    #[test]
    fn blend_hugs_the_road_near_the_edge_and_terrain_far_out() {
        assert_eq!(blend_toward_terrain(10.0, -20.0, 0.0), 10.0);
        assert_eq!(blend_toward_terrain(10.0, -20.0, BLEND_START_M), 10.0);
        assert_eq!(blend_toward_terrain(10.0, -20.0, BLEND_END_M + 50.0), -20.0);
        let mid = blend_toward_terrain(10.0, -20.0, (BLEND_START_M + BLEND_END_M) / 2.0);
        assert!(mid < 10.0 && mid > -20.0);
    }

    #[test]
    fn same_path_builds_an_identical_field() {
        let (a, b) = (field(), field());
        assert_eq!(a.heights, b.heights);
    }
}
