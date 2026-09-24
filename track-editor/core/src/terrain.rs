//! The ground: a coarse terrain heightfield derived from the track's own
//! centerline, plus the verge that ties it to the road.
//!
//! Tracks carry no terrain of their own — only the centerline has heights.
//! Ground bands used to be extruded sideways at their own station's height,
//! which reads fine on a flat circuit and falls apart the moment a track
//! folds back on itself with an elevation change: a 130 m grass band from a
//! high section would hang in mid-air above (or knife through) a lower
//! section 30 m away.
//!
//! The fix is a shared notion of "the ground here", answered for any XY by
//! [`TerrainHeightfield::ground_height_at`]:
//!
//! - Far from every road it is the **terrain field**: every centerline
//!   sample contributes its height to a regular grid via inverse-distance
//!   weighting, so the terrain between two neighbouring track sections
//!   interpolates smoothly between their elevations, capped by a road
//!   ceiling so a hill can never bury a road.
//! - Within [`BLEND_START_M`] of a road edge it is the **verge**: the road
//!   edge's own height less [`VERGE_DROP_M`], all the way round the circuit
//!   and along the pit lane, whether or not a `.ats` band is authored there.
//! - In between it is a smoothstep from the verge into the terrain, reached
//!   by [`BLEND_END_M`] out.
//!
//! Every consumer — the ground mesh, the authored surface bands, curb outer
//! faces, prop seating, the server's ground sidecar — samples that one
//! function, which is what keeps them from stepping against each other.
//!
//! Where the course passes over itself — Suzuka's crossover — a heightfield
//! cannot hold both levels, and the "never bury a road" ceiling used to
//! carve the upper road's ground into a 70 m valley around the lower one,
//! leaving the upper road floating over it. Such a crossing is an
//! [`Underpass`]: behind a wall line [`UNDERPASS_WALL_GAP_M`] past the
//! lower road's edges the lower road stops shaping the ground, so the upper
//! road keeps its embankment and the lower road runs through a slot in it.
//! The step at the wall line is vertical, which a gridded mesh cannot show;
//! the exporter cuts the grid there and draws the walls, the slot floor and
//! the deck itself (`ue_export`).
//!
//! Building the field is deterministic: fixed iteration order, no wall
//! clock, plain `f32` arithmetic — repeated bakes stay byte-identical.

use crate::dem::{DemFile, DemGrid};
use serde::{Deserialize, Serialize};

use crate::track_path::{offset_point, CenterlinePath};

/// Grid spacing of the sampled terrain field, meters. Coarse on purpose:
/// this is rolling ground, not road surface.
const CELL_M: f32 = 12.0;
/// Extra ground beyond the centerline's bounding box, meters. Must cover
/// the widest surface band content ships (130 m) with room to spare.
// Wide enough that, with height fog over it, the mesh edge sits in haze
// instead of cutting a visible line against the sky from driver height.
const MARGIN_M: f32 = 800.0;

/// Where the ground stops being the road's own surveyed height and starts
/// being the elevation model, and where it has finished becoming it.
/// Inside the first figure the trace wins outright, which keeps a circuit
/// in a cutting or on an embankment shaped the way it really is; past the
/// second the model wins outright, which is what puts the hills back.
const DEM_BLEND_START_M: f32 = 40.0;
const DEM_BLEND_END_M: f32 = 220.0;
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

/// How far below the road edge the verge sits, meters. A real road stands
/// a few centimetres proud of the grass beside it; this is also what keeps
/// the verge from z-fighting the road ribbon.
pub const VERGE_DROP_M: f32 = 0.08;

/// Lateral distance beyond the track edge where the ground still follows
/// the road edge exactly, meters…
pub const BLEND_START_M: f32 = 6.0;
/// …and where it has fully blended into the terrain field.
pub const BLEND_END_M: f32 = 35.0;

/// Under the road itself the ground dives below the surface at this grade
/// (m per m in from the edge), capped at [`ROAD_DIVE_MAX_M`]. It starts at
/// verge height right at the edge, so it is continuous with the verge on
/// both sides of a banked road, and it drops away fast enough that a 4 m
/// ground facet straddling the low edge of a banked corner cannot rise up
/// through the road surface.
const ROAD_DIVE_PER_M: f32 = 0.5;
const ROAD_DIVE_MAX_M: f32 = 2.0;

/// Where two roads run close at different heights, the ground near the
/// lower one may not climb toward the upper one faster than this (m per
/// m beyond the lower road's verge). It only ever binds between two roads;
/// a lone road's blend never reaches it.
const CEILING_RISE: f32 = 0.5;

/// Least height between two roads crossing in plan for the crossing to be
/// a bridge rather than two sections that merely meet, meters.
pub const UNDERPASS_CLEARANCE_M: f32 = 4.0;
/// Where the abutment walls stand, meters past the lower road's edges.
pub const UNDERPASS_WALL_GAP_M: f32 = 3.5;
/// How far the bridge deck reaches past the upper road's edges, meters.
pub const DECK_OVERHANG_M: f32 = 1.6;
/// Depth of the deck below the road surface, meters.
pub const DECK_DEPTH_M: f32 = 1.2;
/// A road surface further than this from the height being asked about is
/// another level — a bridge overhead, or the road below one — not the
/// surface there, meters. The ground under a road sits at most
/// `VERGE_DROP_M + ROAD_DIVE_MAX_M` below it, so this never splits a road
/// from its own ground.
pub const OVERHEAD_M: f32 = 3.0;
/// Over this much station at each end of the walls the lower road's say
/// over the ground behind them comes back, meters, so the wall height runs
/// out to nothing instead of stopping at a step.
const UNDERPASS_FADE_M: f32 = 12.0;
/// Farthest along either road an underpass is looked for from its
/// crossing, meters.
const UNDERPASS_REACH_M: f32 = 250.0;

/// Softening in the road weights: a road inside its verge weighs
/// `1 / HUG_EPS` against the terrain's 1, so the verge follows the road to
/// within a millimetre while the weight stays finite (and the blend stays
/// continuous) across the verge boundary.
const HUG_EPS: f32 = 1e-3;

/// Spatial bucket size of the road segment index, meters.
const BUCKET_M: f32 = 16.0;
/// How far past the road bounding box the segment index reaches. Queries
/// outside it simply find no road.
const INDEX_MARGIN_M: f32 = 80.0;
/// Most nearby road runs considered by one ground query.
const MAX_CANDIDATES: usize = 6;

/// Preview / material color for the ground itself: darker than any grass
/// band so authored surfaces still read on top of it.
pub const GROUND_COLOR: [f32; 4] = [0.11, 0.27, 0.10, 1.0];

/// Grid spacing of the server's ground sidecar, meters.
pub const GROUND_SIDECAR_CELL_M: f32 = 4.0;
/// Margin of the sidecar past the road bounding box, meters.
pub const GROUND_SIDECAR_MARGIN_M: f32 = 80.0;
pub const GROUND_SIDECAR_VERSION: u32 = 1;

/// The heightfield the server reads (`<Track>.ground.msgpack`, written
/// with `rmp_serde::to_vec_named`). Row-major, `index = row * cols + col`,
/// sample `(col, row)` at `(origin_x + col * cell_m, origin_y + row *
/// cell_m)`, all in the server's frame (meters, x forward at start/finish,
/// y left, z up — the YAML's frame, not Unreal's). Each sample is the
/// surface the client renders at that XY: the road (with banking) inside
/// the road, the pit lane inside the pit lane, the verge / band / terrain
/// ground elsewhere. Heights in centimeters, clamped to the `i16` range.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct GroundHeightfield {
    pub version: u32,
    pub origin_x: f32,
    pub origin_y: f32,
    pub cell_m: f32,
    pub cols: u32,
    pub rows: u32,
    pub heights_cm: Vec<i16>,
}

impl GroundHeightfield {
    /// Height at sample `(col, row)`, meters.
    pub fn height_m(&self, col: u32, row: u32) -> f32 {
        self.heights_cm[(row * self.cols + col) as usize] as f32 / 100.0
    }
}

/// One road-like polyline the ground hugs: the track itself, the pit lane.
struct Road {
    path: CenterlinePath,
    /// Widest half-width along it, for the search radius.
    max_half_m: f32,
}

/// Uniform buckets over the road segments, so a ground query only looks at
/// the handful of segments near it instead of every one on the circuit.
/// CSR layout: `entries[offsets[b]..offsets[b + 1]]` are bucket `b`'s
/// `(road, segment)` pairs, in `(road, segment)` order.
struct SegmentIndex {
    origin: (f32, f32),
    cols: usize,
    rows: usize,
    offsets: Vec<u32>,
    entries: Vec<(u32, u32)>,
}

/// A road segment near a query point, with the point resolved against it.
#[derive(Clone, Copy, Debug)]
struct Hit {
    road: u32,
    segment: u32,
    /// 2D distance from the point to its foot on the segment, meters.
    dist: f32,
    /// Station of that foot along the road.
    station: f32,
    /// Signed lateral: `dist` with the sign of the road's left/right.
    lat: f32,
}

/// What one nearby road contributes to the ground at a point.
struct Contribution {
    /// The height the ground would have following this road alone.
    hug: f32,
    /// Its weight against the terrain's weight of 1.
    weight: f32,
    /// The ground may not rise above this on this road's account.
    ceiling: f32,
    /// The road surface at the point, when the point is on the road.
    surface: Option<f32>,
    /// Distance past this road's edge on the point's side; negative on the
    /// road.
    beyond: f32,
}

/// One place the course passes over itself (or the pit lane over the
/// course): the lower road runs through a slot in the upper road's
/// embankment, under a deck. Stations of both spans are unwrapped — `.0 <=
/// .1`, and on a closed road they may run past the total length.
#[derive(Clone, Debug, PartialEq)]
pub struct Underpass {
    /// Road index (0 = the track, then any pit lane) passing below.
    pub lower_road: u32,
    pub upper_road: u32,
    /// Stations of the crossing point on each road.
    pub lower_station_m: f32,
    pub upper_station_m: f32,
    /// Where the centerlines cross, track space.
    pub at: (f32, f32),
    /// Height of the upper road over the lower one at the crossing, meters.
    pub clearance_m: f32,
    /// Stretch of the lower road with the embankment beside it: the walls.
    pub wall_span_m: (f32, f32),
    /// Stretch of the upper road carried over the slot: the deck.
    pub deck_span_m: (f32, f32),
}

pub struct TerrainHeightfield {
    /// Track-space position of grid vertex (0, 0).
    origin: (f32, f32),
    cell_m: f32,
    /// Grid vertex counts (not cell counts).
    cols: usize,
    rows: usize,
    /// Row-major heights, `rows * cols` entries.
    heights: Vec<f32>,

    roads: Vec<Road>,
    index: SegmentIndex,
    /// Radius within which a road can still shape the ground.
    search_radius_m: f32,
    /// Bounding box of every road including its width: `(min_x, min_y,
    /// max_x, max_y)`.
    road_bounds: (f32, f32, f32, f32),
    underpasses: Vec<Underpass>,
    /// The elevation model's far grid, for queries past this field's own
    /// edge. Without it anything beyond the 800 m margin read the border
    /// height, so a prop seated two kilometres out — the woodland on the
    /// slopes — came out on a flat plain at road level while the horizon
    /// mesh drew the real hill a few hundred metres above it.
    far: Option<DemGrid>,
}

impl TerrainHeightfield {
    /// Build the field for a sampled centerline. Returns `None` for a
    /// degenerate path (no samples, or a bounding box of zero extent).
    pub fn from_path(path: &CenterlinePath) -> Option<Self> {
        Self::from_paths(path, &[])
    }

    /// [`Self::from_path`] with further road-like paths — the pit lane —
    /// that the ground hugs exactly like the track.
    pub fn from_paths(path: &CenterlinePath, extra: &[&CenterlinePath]) -> Option<Self> {
        Self::from_paths_with_dem(path, extra, None)
    }

    /// [`Self::from_paths`] over the real land.
    ///
    /// With an elevation model the ground is the land, and the
    /// centerline's own heights are used only to carve the road into it.
    /// Without one it is the inverse-distance blanket it always was: a
    /// smooth surface that decays to the mean track height, with no hill
    /// or valley that the road does not itself imply. That fallback is
    /// what every circuit looked like, and it is why the Red Bull Ring
    /// sat in a green pancake instead of the Murtal.
    pub fn from_paths_with_dem(
        path: &CenterlinePath,
        extra: &[&CenterlinePath],
        dem: Option<&DemFile>,
    ) -> Option<Self> {
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
        let mut all_paths: Vec<&CenterlinePath> = vec![path];
        all_paths.extend(extra.iter().copied());
        for p in &all_paths {
            let mut next_station = 0.0f32;
            for s in p.samples() {
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
                let mut nearest2 = f32::MAX;
                for s in &sources {
                    let d2 = (x - s.x).powi(2) + (y - s.y).powi(2);
                    let w = 1.0 / (d2 + IDW_SOFTENING_M2);
                    num += s.z * w;
                    den += w;
                    nearest2 = nearest2.min(d2);
                    let allowance = (d2.sqrt() - s.reach_m).max(0.0) * RISE_SLOPE;
                    ceiling = ceiling.min(s.ceiling_z + allowance);
                }
                let blanket = num / den;
                let height = match dem {
                    // Close to the road the surveyed centerline is the
                    // better of the two: a 30 m elevation model reads the
                    // tree canopy over a tree-lined circuit and knows
                    // nothing of cuttings and embankments, while the
                    // trace is the road itself. Further out the model is
                    // the only thing that knows there is a hill there at
                    // all, so the two are crossfaded over the same band.
                    Some(dem) => {
                        let t = ((nearest2.sqrt() - DEM_BLEND_START_M)
                            / (DEM_BLEND_END_M - DEM_BLEND_START_M))
                            .clamp(0.0, 1.0);
                        let eased = t * t * (3.0 - 2.0 * t);
                        let land = blanket + (dem.height_at(x, y) - blanket) * eased;
                        // The ceiling fades out across the same band.
                        //
                        // It exists so that no road is ever buried, and it
                        // does that by letting the ground rise away from a
                        // road at no more than `RISE_SLOPE`, 15 %. Over the
                        // blanket that never bit. Over real land it clipped
                        // every hill steeper than that: the Red Bull Ring's
                        // slopes rise 25 %, and the first export with the
                        // elevation model peaked at 196 m where the land
                        // reaches 408 m — the ceiling had quietly flattened
                        // the mountains this change was for. Near the road
                        // the ceiling still holds absolutely; a kilometre
                        // out there is no road for it to protect.
                        let clamped = land.min(ceiling);
                        clamped + (land - clamped) * eased
                    }
                    // The land is the shape of the ground; the ceiling is
                    // the guarantee that no road is ever buried by it.
                    None => blanket.min(ceiling),
                };
                heights.push(height);
            }
        }

        let roads: Vec<Road> = all_paths
            .iter()
            .map(|p| Road {
                path: (*p).clone(),
                max_half_m: p
                    .samples()
                    .iter()
                    .map(|s| s.width_left_m.max(s.width_right_m))
                    .fold(0.0, f32::max),
            })
            .collect();
        let max_half = roads.iter().map(|r| r.max_half_m).fold(0.0, f32::max);
        let road_bounds = (
            min_x - max_half,
            min_y - max_half,
            max_x + max_half,
            max_y + max_half,
        );
        let index = SegmentIndex::build(&roads, road_bounds);

        let mut field = Self {
            origin,
            cell_m: CELL_M,
            cols,
            rows,
            heights,
            roads,
            index,
            search_radius_m: BLEND_END_M + max_half + 1.0,
            road_bounds,
            underpasses: Vec::new(),
            far: dem.map(|d| d.outer.clone()),
        };
        field.underpasses = field.find_underpasses();
        Some(field)
    }

    /// Raw terrain height at a track-space position — the far field, with
    /// no verge: bilinear between grid vertices, clamped to the grid at its
    /// borders. Most callers want [`Self::ground_height_at`].
    pub fn height_at(&self, x: f32, y: f32) -> f32 {
        if let Some(far) = &self.far {
            let (max_x, max_y) = (
                self.origin.0 + (self.cols - 1) as f32 * self.cell_m,
                self.origin.1 + (self.rows - 1) as f32 * self.cell_m,
            );
            if x < self.origin.0 || y < self.origin.1 || x > max_x || y > max_y {
                return far.height_at(x, y);
            }
        }
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

    /// The visible ground height at a track-space position: the verge
    /// beside any road (track or pit lane), blending into the terrain
    /// field further out, and never rising through a road passing over.
    /// Under a road it dips below the surface; see [`Self::surface_height_at`]
    /// for the surface a car drives on.
    pub fn ground_height_at(&self, x: f32, y: f32) -> f32 {
        self.probe(x, y, UNDERPASS_WALL_GAP_M).ground
    }

    /// The surface at a track-space position: the road (with banking)
    /// inside the road, the pit lane inside the pit lane, and otherwise the
    /// ground of [`Self::ground_height_at`]. Where roads overlap at about
    /// the same height the higher one wins; a road passing overhead is not
    /// the surface under it.
    pub fn surface_height_at(&self, x: f32, y: f32) -> f32 {
        let probe = self.probe(x, y, UNDERPASS_WALL_GAP_M);
        probe.road_near(probe.ground).unwrap_or(probe.ground)
    }

    /// [`Self::surface_height_at`] for something that knows roughly how
    /// high it is: of the road surfaces at the point, the level nearest
    /// `reference_z`. On a bridge that is the bridge, its deck past the road
    /// edges included; under it, the road below.
    pub fn surface_height_near(&self, x: f32, y: f32, reference_z: f32) -> f32 {
        let probe = self.probe(x, y, UNDERPASS_WALL_GAP_M);
        probe
            .road_near(reference_z)
            .or_else(|| {
                self.deck_top_at(x, y)
                    .filter(|top| (top - reference_z).abs() <= OVERHEAD_M)
            })
            .unwrap_or(probe.ground)
    }

    /// Ground and road surfaces at a point, from one candidate search, with
    /// the underpass walls `wall_gap` past the lower road's edges.
    fn probe(&self, x: f32, y: f32, wall_gap: f32) -> Probe {
        let terrain = self.height_at(x, y);
        let mut probe = Probe {
            ground: terrain,
            surfaces: [0.0; MAX_CANDIDATES],
            surface_count: 0,
        };
        let candidates = self.candidates(x, y, self.search_radius_m);
        if candidates.is_empty() {
            return probe;
        }

        let mut num = terrain;
        let mut den = 1.0f32;
        let mut ceiling = f32::INFINITY;
        for hit in &candidates {
            let mut c = self.contribution(hit);
            if let Some((k, relief)) = self.behind_wall(hit, c.beyond, wall_gap) {
                // Behind an underpass wall the ground is the upper road's
                // embankment: the lower road gives up its pull on it and
                // its ceiling, fully once past the fade at the walls' ends.
                c.weight *= 1.0 - k;
                c.ceiling += k * relief;
            }
            num += c.hug * c.weight;
            den += c.weight;
            ceiling = ceiling.min(c.ceiling);
            if let Some(s) = c.surface {
                probe.surfaces[probe.surface_count] = s;
                probe.surface_count += 1;
            }
        }
        probe.ground = (num / den).min(ceiling);
        probe
    }

    /// Whether `hit` is on the far side of an underpass wall from its road,
    /// and if so how much of the ground there the wall takes over (1 along
    /// the walls, easing to 0 at their ends) and how far the road's ceiling
    /// must lift to stop binding.
    fn behind_wall(&self, hit: &Hit, beyond: f32, wall_gap: f32) -> Option<(f32, f32)> {
        if beyond <= wall_gap || self.underpasses.is_empty() {
            return None;
        }
        let total = self.roads[hit.road as usize].path.total_length_m();
        self.underpasses
            .iter()
            .filter(|u| u.lower_road == hit.road)
            .find_map(|u| {
                let into = span_offset(u.wall_span_m, hit.station, total)?;
                let length = u.wall_span_m.1 - u.wall_span_m.0;
                let t = (into.min(length - into) / UNDERPASS_FADE_M).clamp(0.0, 1.0);
                let k = t * t * (3.0 - 2.0 * t);
                (k > 0.0).then_some((k, u.clearance_m + 10.0))
            })
    }

    /// The underpasses in this field's roads, ordered by lower road and
    /// station.
    pub fn underpasses(&self) -> &[Underpass] {
        &self.underpasses
    }

    /// Road `road`'s centerline: 0 is the track, then the extra paths in the
    /// order given to [`Self::from_paths`].
    pub fn road_path(&self, road: u32) -> &CenterlinePath {
        &self.roads[road as usize].path
    }

    /// Height of the bridge deck's top — the upper road's surface at the
    /// nearest point of the road — over a point under a deck, its overhang
    /// past the road edges included.
    pub fn deck_top_at(&self, x: f32, y: f32) -> Option<f32> {
        self.underpasses
            .iter()
            .filter(|u| u.reaches(x, y))
            .find_map(|u| {
                let upper = &self.roads[u.upper_road as usize].path;
                let hit = self.nearest_on_road(u.upper_road, u.upper_station_m, x, y)?;
                span_offset(u.deck_span_m, hit.station, upper.total_length_m())?;
                let sample = upper.sample_at(hit.station);
                let half = if hit.lat >= 0.0 {
                    sample.width_left_m
                } else {
                    sample.width_right_m
                };
                (hit.dist <= half + DECK_OVERHANG_M).then(|| {
                    let lat = hit.lat.clamp(-sample.width_right_m, sample.width_left_m);
                    offset_point(&sample, lat).2
                })
            })
    }

    /// Where a point sits relative to the underpass walls: `None` away from
    /// every wall, else how far past the wall line it is (negative inside
    /// the slot) and the lower road's centerline height there.
    pub fn wall_relation(&self, x: f32, y: f32) -> Option<(f32, f32)> {
        self.underpasses
            .iter()
            .filter(|u| u.reaches(x, y))
            .find_map(|u| {
                let lower = &self.roads[u.lower_road as usize].path;
                let hit = self.nearest_on_road(u.lower_road, u.lower_station_m, x, y)?;
                span_offset(u.wall_span_m, hit.station, lower.total_length_m())?;
                let sample = lower.sample_at(hit.station);
                let half = if hit.lat >= 0.0 {
                    sample.width_left_m
                } else {
                    sample.width_right_m
                };
                Some((hit.dist - half - UNDERPASS_WALL_GAP_M, sample.pos.2))
            })
    }

    /// The sidecar's surface: [`Self::surface_height_at`] with the walls
    /// pushed one diagonal of the sidecar grid further out. A wall is a
    /// vertical step, and a bilinear sample within a cell of one already
    /// climbs part of the way up it: a car a metre wide of the edge under
    /// the bridge would ride up a ramp that isn't there.
    fn physics_surface_at(&self, x: f32, y: f32, cell_m: f32) -> f32 {
        let probe = self.probe(
            x,
            y,
            UNDERPASS_WALL_GAP_M + cell_m * std::f32::consts::SQRT_2,
        );
        probe.road_near(probe.ground).unwrap_or(probe.ground)
    }

    /// The nearest point of road `road` to `(x, y)`, searching only the
    /// stretch within reach of `station_m` so that the other level at the
    /// same XY is never the answer.
    fn nearest_on_road(&self, road: u32, station_m: f32, x: f32, y: f32) -> Option<Hit> {
        let r = &self.roads[road as usize];
        let radius = r.max_half_m + UNDERPASS_WALL_GAP_M + BLEND_END_M;
        let total = r.path.total_length_m();
        self.index
            .gather(&self.roads, x, y, radius)
            .into_iter()
            .filter(|h| {
                h.road == road && station_gap(h.station, station_m, total) <= UNDERPASS_REACH_M
            })
            .min_by(|a, b| a.dist.total_cmp(&b.dist).then(a.segment.cmp(&b.segment)))
    }

    /// Every place a road crosses a road (itself included) with at least
    /// [`UNDERPASS_CLEARANCE_M`] between the two, with the spans of its
    /// walls and deck.
    fn find_underpasses(&self) -> Vec<Underpass> {
        let mut found: Vec<Underpass> = Vec::new();
        for (ri, road) in self.roads.iter().enumerate() {
            let total = road.path.total_length_m();
            for i in 0..segment_count(&road.path) {
                let (a, b, sa, sb) = segment_ends(&road.path, i);
                let len = ((b.0 - a.0).powi(2) + (b.1 - a.1).powi(2)).sqrt();
                let mid = ((a.0 + b.0) * 0.5, (a.1 + b.1) * 0.5);
                let mut others = self
                    .index
                    .gather(&self.roads, mid.0, mid.1, len * 0.5 + 0.5);
                others.sort_unstable_by_key(|h| (h.road, h.segment));
                others.dedup_by_key(|h| (h.road, h.segment));
                for other in others {
                    // Each pair once, and never a segment against its own
                    // neighbourhood on the same road.
                    if (other.road as usize, other.segment as usize) <= (ri, i) {
                        continue;
                    }
                    let other_path = &self.roads[other.road as usize].path;
                    let (c, d, sc, sd) = segment_ends(other_path, other.segment as usize);
                    if other.road as usize == ri && station_gap(sa, sc, total) < 2.0 * BLEND_END_M {
                        continue;
                    }
                    let Some((t, u)) = segment_intersection(a, b, c, d) else {
                        continue;
                    };
                    let z1 = a.2 + (b.2 - a.2) * t;
                    let z2 = c.2 + (d.2 - c.2) * u;
                    if (z1 - z2).abs() < UNDERPASS_CLEARANCE_M {
                        continue;
                    }
                    let s1 = sa + (sb - sa) * t;
                    let s2 = sc + (sd - sc) * u;
                    let ((lower, ls), (upper, us)) = if z1 < z2 {
                        ((ri as u32, s1), (other.road, s2))
                    } else {
                        ((other.road, s2), (ri as u32, s1))
                    };
                    let lower_total = self.roads[lower as usize].path.total_length_m();
                    let duplicate = found.iter().any(|f| {
                        f.lower_road == lower
                            && f.upper_road == upper
                            && station_gap(f.lower_station_m, ls, lower_total) < 10.0
                    });
                    if duplicate {
                        continue;
                    }
                    found.push(Underpass {
                        lower_road: lower,
                        upper_road: upper,
                        lower_station_m: ls,
                        upper_station_m: us,
                        at: (a.0 + (b.0 - a.0) * t, a.1 + (b.1 - a.1) * t),
                        clearance_m: (z1 - z2).abs(),
                        wall_span_m: (ls, ls),
                        deck_span_m: (us, us),
                    });
                }
            }
        }
        // Spans use `nearest_on_road`, which needs the crossing stations
        // but not the spans themselves.
        let spans: Vec<_> = found
            .iter()
            .map(|u| (self.wall_span(u), self.deck_span(u)))
            .collect();
        for (u, (walls, deck)) in found.iter_mut().zip(spans) {
            u.wall_span_m = walls;
            u.deck_span_m = deck;
        }
        found.sort_by(|a, b| {
            a.lower_road
                .cmp(&b.lower_road)
                .then(a.lower_station_m.total_cmp(&b.lower_station_m))
        });
        found
    }

    /// The stretch of the lower road whose wall lines are within the upper
    /// road's verge blend — where its embankment stands — plus the fade.
    fn wall_span(&self, u: &Underpass) -> (f32, f32) {
        const STEP_M: f32 = 2.0;
        let lower = &self.roads[u.lower_road as usize].path;
        let upper = &self.roads[u.upper_road as usize].path;
        let reaches = |station: f32| {
            let s = lower.sample_at(station);
            [
                s.width_left_m + UNDERPASS_WALL_GAP_M,
                -(s.width_right_m + UNDERPASS_WALL_GAP_M),
            ]
            .into_iter()
            .any(|lat| {
                let p = offset_point(&s, lat);
                self.nearest_on_road(u.upper_road, u.upper_station_m, p.0, p.1)
                    .is_some_and(|h| {
                        let us = upper.sample_at(h.station);
                        h.dist - us.width_left_m.max(us.width_right_m) < BLEND_END_M
                    })
            })
        };
        let walk = |dir: f32| {
            let mut last = 0.0f32;
            let mut d = STEP_M;
            while d <= UNDERPASS_REACH_M && d - last <= 3.0 * STEP_M {
                if reaches(u.lower_station_m + dir * d) {
                    last = d;
                }
                d += STEP_M;
            }
            last + UNDERPASS_FADE_M
        };
        clamp_span(
            lower,
            u.lower_station_m - walk(-1.0),
            u.lower_station_m + walk(1.0),
        )
    }

    /// The stretch of the upper road whose deck, overhang included, is over
    /// the slot or bearing on the walls either side of it.
    fn deck_span(&self, u: &Underpass) -> (f32, f32) {
        const STEP_M: f32 = 0.5;
        /// How far the deck rests on the ground past each wall line.
        const BEARING_M: f32 = 1.0;
        let upper = &self.roads[u.upper_road as usize].path;
        let lower = &self.roads[u.lower_road as usize].path;
        let over = |station: f32| {
            let s = upper.sample_at(station);
            let left = s.width_left_m + DECK_OVERHANG_M;
            let right = -(s.width_right_m + DECK_OVERHANG_M);
            (0..=8).any(|k| {
                let lat = left + (right - left) * (k as f32 / 8.0);
                let p = offset_point(&s, lat);
                self.nearest_on_road(u.lower_road, u.lower_station_m, p.0, p.1)
                    .is_some_and(|h| {
                        let ls = lower.sample_at(h.station);
                        let half = if h.lat >= 0.0 {
                            ls.width_left_m
                        } else {
                            ls.width_right_m
                        };
                        h.dist - half <= UNDERPASS_WALL_GAP_M + BEARING_M
                    })
            })
        };
        let walk = |dir: f32| {
            let mut d = 0.0f32;
            while d < UNDERPASS_REACH_M && over(u.upper_station_m + dir * (d + STEP_M)) {
                d += STEP_M;
            }
            d
        };
        clamp_span(
            upper,
            u.upper_station_m - walk(-1.0),
            u.upper_station_m + walk(1.0),
        )
    }

    /// Evaluate one nearby road's say over the ground at the query point.
    fn contribution(&self, hit: &Hit) -> Contribution {
        let road = &self.roads[hit.road as usize];
        let sample = road.path.sample_at(hit.station);
        let (half, edge_lat) = if hit.lat >= 0.0 {
            (sample.width_left_m, sample.width_left_m)
        } else {
            (sample.width_right_m, -sample.width_right_m)
        };
        let beyond = hit.dist - half;
        let edge_z = offset_point(&sample, edge_lat).2;

        if beyond < 0.0 {
            // On the road: follow the surface (banking included) and dive
            // under it, continuous with the verge at the edge.
            let surface_z = offset_point(&sample, hit.lat).2;
            let depth_in = (-beyond).min(sample.width_left_m + sample.width_right_m + beyond);
            let dive = (depth_in * ROAD_DIVE_PER_M).min(ROAD_DIVE_MAX_M);
            let hug = surface_z - VERGE_DROP_M - dive;
            return Contribution {
                hug,
                weight: 1.0 / HUG_EPS,
                ceiling: hug,
                surface: Some(surface_z),
                beyond,
            };
        }

        let hug = edge_z - VERGE_DROP_M;
        let t = ((beyond - BLEND_START_M) / (BLEND_END_M - BLEND_START_M)).clamp(0.0, 1.0);
        // Smoothstep, so the ground leaves the verge with zero slope. With
        // a single road the weighted mean below reduces to exactly
        // `hug + (terrain - hug) * t`.
        let t = t * t * (3.0 - 2.0 * t);
        let w = 1.0 - t;
        Contribution {
            hug,
            weight: w / (1.0 - w + HUG_EPS),
            ceiling: hug + (beyond - BLEND_START_M).max(0.0) * CEILING_RISE,
            surface: None,
            beyond,
        }
    }

    /// Nearest road runs to a point, at most [`MAX_CANDIDATES`], nearest
    /// first. A *run* is a stretch of consecutive segments of one road
    /// within `radius`; a hairpin yields two, a straight one. Each run is
    /// represented by its nearest segment.
    fn candidates(&self, x: f32, y: f32, radius: f32) -> Vec<Hit> {
        let mut hits = self.index.gather(&self.roads, x, y, radius);
        if hits.len() <= 1 {
            return hits;
        }
        hits.sort_unstable_by_key(|h| (h.road, h.segment));
        // A segment straddling several buckets was reported once per bucket.
        hits.dedup_by_key(|h| (h.road, h.segment));

        let mut runs: Vec<Hit> = Vec::new();
        let mut best: Option<Hit> = None;
        let mut prev: Option<(u32, u32)> = None;
        for hit in hits {
            let contiguous =
                matches!(prev, Some((road, seg)) if road == hit.road && hit.segment <= seg + 1);
            if !contiguous {
                if let Some(b) = best.take() {
                    runs.push(b);
                }
            }
            best = match best {
                Some(b) if b.dist <= hit.dist => Some(b),
                _ => Some(hit),
            };
            prev = Some((hit.road, hit.segment));
        }
        if let Some(b) = best {
            runs.push(b);
        }

        runs.sort_by(|a, b| {
            a.dist
                .total_cmp(&b.dist)
                .then((a.road, a.segment).cmp(&(b.road, b.segment)))
        });
        runs.truncate(MAX_CANDIDATES);
        runs
    }

    /// 2D distance from a point to the nearest road centerline within
    /// `radius`, if any road is that close.
    pub fn road_distance_at(&self, x: f32, y: f32, radius: f32) -> Option<f32> {
        self.index
            .gather(&self.roads, x, y, radius)
            .iter()
            .map(|h| h.dist)
            .fold(None, |acc: Option<f32>, d| {
                Some(acc.map_or(d, |a| a.min(d)))
            })
    }

    /// The nearest point of the *track* (not the pit lane) within
    /// `radius`: `(station, signed lateral, half-width on that side)`.
    pub fn nearest_track_point(&self, x: f32, y: f32, radius: f32) -> Option<(f32, f32, f32)> {
        let hit = self
            .index
            .gather(&self.roads, x, y, radius)
            .into_iter()
            .filter(|h| h.road == 0)
            .min_by(|a, b| a.dist.total_cmp(&b.dist).then(a.segment.cmp(&b.segment)))?;
        let sample = self.roads[0].path.sample_at(hit.station);
        let half = if hit.lat >= 0.0 {
            sample.width_left_m
        } else {
            sample.width_right_m
        };
        Some((hit.station, hit.lat, half))
    }

    /// The nearest point of any road — the track is road `0`, the pit lane
    /// and any other extra path follow — within `radius`:
    /// `(road, station, signed lateral, half-width on that side)`.
    pub fn nearest_road_point(&self, x: f32, y: f32, radius: f32) -> Option<(u32, f32, f32, f32)> {
        let hit = self
            .index
            .gather(&self.roads, x, y, radius)
            .into_iter()
            .min_by(|a, b| {
                a.dist
                    .total_cmp(&b.dist)
                    .then((a.road, a.segment).cmp(&(b.road, b.segment)))
            })?;
        let sample = self.roads[hit.road as usize].path.sample_at(hit.station);
        let half = if hit.lat >= 0.0 {
            sample.width_left_m
        } else {
            sample.width_right_m
        };
        Some((hit.road, hit.station, hit.lat, half))
    }

    /// Upward ground normal at a point, from central differences of
    /// [`Self::ground_height_at`] over `h` meters.
    pub fn ground_normal_at(&self, x: f32, y: f32, h: f32) -> (f32, f32, f32) {
        let dx = (self.ground_height_at(x + h, y) - self.ground_height_at(x - h, y)) / (2.0 * h);
        let dy = (self.ground_height_at(x, y + h) - self.ground_height_at(x, y - h)) / (2.0 * h);
        let len = (dx * dx + dy * dy + 1.0).sqrt();
        (-dx / len, -dy / len, 1.0 / len)
    }

    /// Track-space position of grid vertex `(col, row)`.
    pub fn vertex(&self, col: usize, row: usize) -> (f32, f32, f32) {
        (
            self.origin.0 + col as f32 * self.cell_m,
            self.origin.1 + row as f32 * self.cell_m,
            self.heights[row * self.cols + col],
        )
    }

    /// Outward (up) surface normal of the raw terrain at grid vertex
    /// `(col, row)`, track space, from central differences of the stored
    /// heights.
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

    /// Bounding box of every road including its width:
    /// `(min_x, min_y, max_x, max_y)`, meters.
    pub fn road_bounds(&self) -> (f32, f32, f32, f32) {
        self.road_bounds
    }

    /// Sample [`Self::surface_height_at`] onto the server's grid: the road
    /// bounding box plus [`GROUND_SIDECAR_MARGIN_M`], `cell_m` apart.
    pub fn bake_ground_sidecar(&self, cell_m: f32) -> GroundHeightfield {
        let (min_x, min_y, max_x, max_y) = self.road_bounds;
        let origin_x = min_x - GROUND_SIDECAR_MARGIN_M;
        let origin_y = min_y - GROUND_SIDECAR_MARGIN_M;
        let cols = ((max_x + GROUND_SIDECAR_MARGIN_M - origin_x) / cell_m).ceil() as u32 + 1;
        let rows = ((max_y + GROUND_SIDECAR_MARGIN_M - origin_y) / cell_m).ceil() as u32 + 1;
        let mut heights_cm = Vec::with_capacity((cols * rows) as usize);
        for r in 0..rows {
            let y = origin_y + r as f32 * cell_m;
            for c in 0..cols {
                let x = origin_x + c as f32 * cell_m;
                let z = self.physics_surface_at(x, y, cell_m);
                let cm = if z.is_finite() {
                    (z * 100.0).round()
                } else {
                    0.0
                };
                heights_cm.push(cm.clamp(i16::MIN as f32, i16::MAX as f32) as i16);
            }
        }
        GroundHeightfield {
            version: GROUND_SIDECAR_VERSION,
            origin_x,
            origin_y,
            cell_m,
            cols,
            rows,
            heights_cm,
        }
    }
}

impl SegmentIndex {
    fn build(roads: &[Road], bounds: (f32, f32, f32, f32)) -> Self {
        let origin = (bounds.0 - INDEX_MARGIN_M, bounds.1 - INDEX_MARGIN_M);
        let cols = (((bounds.2 + INDEX_MARGIN_M - origin.0) / BUCKET_M).ceil() as usize).max(1);
        let rows = (((bounds.3 + INDEX_MARGIN_M - origin.1) / BUCKET_M).ceil() as usize).max(1);

        let bucket_of = |x: f32, y: f32| -> (usize, usize) {
            let c = (((x - origin.0) / BUCKET_M).floor().max(0.0) as usize).min(cols - 1);
            let r = (((y - origin.1) / BUCKET_M).floor().max(0.0) as usize).min(rows - 1);
            (c, r)
        };

        // Every bucket a segment's bounding box touches gets the segment.
        let mut placements: Vec<(usize, (u32, u32))> = Vec::new();
        for (ri, road) in roads.iter().enumerate() {
            let samples = road.path.samples();
            let count = segment_count(&road.path);
            for i in 0..count {
                let a = samples[i].pos;
                let b = samples[(i + 1) % samples.len()].pos;
                let (c0, r0) = bucket_of(a.0.min(b.0), a.1.min(b.1));
                let (c1, r1) = bucket_of(a.0.max(b.0), a.1.max(b.1));
                for r in r0..=r1 {
                    for c in c0..=c1 {
                        placements.push((r * cols + c, (ri as u32, i as u32)));
                    }
                }
            }
        }
        placements.sort_unstable();

        let mut offsets = vec![0u32; cols * rows + 1];
        for (bucket, _) in &placements {
            offsets[bucket + 1] += 1;
        }
        for b in 0..cols * rows {
            offsets[b + 1] += offsets[b];
        }
        let entries = placements.into_iter().map(|(_, e)| e).collect();

        Self {
            origin,
            cols,
            rows,
            offsets,
            entries,
        }
    }

    /// Every segment within `radius` of `(x, y)`, resolved against the
    /// point. Order is by bucket then `(road, segment)`; deterministic.
    fn gather(&self, roads: &[Road], x: f32, y: f32, radius: f32) -> Vec<Hit> {
        let mut hits = Vec::new();
        let lo_c = ((x - radius - self.origin.0) / BUCKET_M).floor();
        let hi_c = ((x + radius - self.origin.0) / BUCKET_M).floor();
        let lo_r = ((y - radius - self.origin.1) / BUCKET_M).floor();
        let hi_r = ((y + radius - self.origin.1) / BUCKET_M).floor();
        if hi_c < 0.0 || hi_r < 0.0 || lo_c >= self.cols as f32 || lo_r >= self.rows as f32 {
            return hits;
        }
        let lo_c = lo_c.max(0.0) as usize;
        let hi_c = (hi_c as usize).min(self.cols - 1);
        let lo_r = lo_r.max(0.0) as usize;
        let hi_r = (hi_r as usize).min(self.rows - 1);

        for r in lo_r..=hi_r {
            for c in lo_c..=hi_c {
                let b = r * self.cols + c;
                for &(road_i, seg) in
                    &self.entries[self.offsets[b] as usize..self.offsets[b + 1] as usize]
                {
                    let road = &roads[road_i as usize];
                    let samples = road.path.samples();
                    let i = seg as usize;
                    let a = samples[i];
                    let (b_pos, b_station) = if i + 1 < samples.len() {
                        (samples[i + 1].pos, samples[i + 1].station_m)
                    } else {
                        (samples[0].pos, road.path.total_length_m())
                    };
                    let (ex, ey) = (b_pos.0 - a.pos.0, b_pos.1 - a.pos.1);
                    let (px, py) = (x - a.pos.0, y - a.pos.1);
                    let len2 = ex * ex + ey * ey;
                    let t = if len2 > 1e-9 {
                        ((px * ex + py * ey) / len2).clamp(0.0, 1.0)
                    } else {
                        0.0
                    };
                    let (fx, fy) = (px - ex * t, py - ey * t);
                    let dist = (fx * fx + fy * fy).sqrt();
                    if dist > radius {
                        continue;
                    }
                    // Left of the course is positive lateral.
                    let left = ex * py - ey * px >= 0.0;
                    hits.push(Hit {
                        road: road_i,
                        segment: seg,
                        dist,
                        station: a.station_m + (b_station - a.station_m) * t,
                        lat: if left { dist } else { -dist },
                    });
                }
            }
        }
        hits
    }
}

impl Underpass {
    /// Whether a point is close enough to the crossing for its walls or
    /// deck to matter: a cheap test before any road search.
    fn reaches(&self, x: f32, y: f32) -> bool {
        let r = UNDERPASS_REACH_M + UNDERPASS_FADE_M + BLEND_END_M + 20.0;
        (x - self.at.0).powi(2) + (y - self.at.1).powi(2) <= r * r
    }
}

/// What one ground query found.
struct Probe {
    ground: f32,
    /// Road surfaces at the point, one per road run it is on.
    surfaces: [f32; MAX_CANDIDATES],
    surface_count: usize,
}

impl Probe {
    /// The road surface at the point at the level asked about, if a road is
    /// there: only roads within [`OVERHEAD_M`] of `reference` count, the
    /// level nearest it wins, and of roads overlapping at that level the
    /// highest.
    fn road_near(&self, reference: f32) -> Option<f32> {
        let surfaces = &self.surfaces[..self.surface_count];
        let nearest = surfaces
            .iter()
            .copied()
            .filter(|s| (s - reference).abs() <= OVERHEAD_M)
            .min_by(|a, b| (a - reference).abs().total_cmp(&(b - reference).abs()))?;
        Some(
            surfaces
                .iter()
                .copied()
                .filter(|s| (s - nearest).abs() <= OVERHEAD_M)
                .fold(nearest, f32::max),
        )
    }
}

/// A track-space position, meters.
type Point = (f32, f32, f32);

/// Endpoints and stations of segment `i` of a path; the closing segment of
/// a closed path ends at the total length.
fn segment_ends(path: &CenterlinePath, i: usize) -> (Point, Point, f32, f32) {
    let samples = path.samples();
    let a = samples[i];
    if i + 1 < samples.len() {
        (
            a.pos,
            samples[i + 1].pos,
            a.station_m,
            samples[i + 1].station_m,
        )
    } else {
        (a.pos, samples[0].pos, a.station_m, path.total_length_m())
    }
}

/// Parameters `(t, u)` along `a-b` and `c-d` where the two cross in plan.
fn segment_intersection(
    a: (f32, f32, f32),
    b: (f32, f32, f32),
    c: (f32, f32, f32),
    d: (f32, f32, f32),
) -> Option<(f32, f32)> {
    let r = (b.0 - a.0, b.1 - a.1);
    let q = (d.0 - c.0, d.1 - c.1);
    let den = r.0 * q.1 - r.1 * q.0;
    if den.abs() < 1e-9 {
        return None;
    }
    let (wx, wy) = (c.0 - a.0, c.1 - a.1);
    let t = (wx * q.1 - wy * q.0) / den;
    let u = (wx * r.1 - wy * r.0) / den;
    ((0.0..=1.0).contains(&t) && (0.0..=1.0).contains(&u)).then_some((t, u))
}

/// Distance between two stations along a road, the short way round a loop.
fn station_gap(a: f32, b: f32, total: f32) -> f32 {
    if total > 0.0 {
        let d = (a - b).abs().rem_euclid(total);
        d.min(total - d)
    } else {
        (a - b).abs()
    }
}

/// How far `station` is into an unwrapped `span`, trying it a lap either
/// way round; `None` outside the span.
fn span_offset(span: (f32, f32), station: f32, total: f32) -> Option<f32> {
    [station, station + total, station - total]
        .into_iter()
        .find(|s| *s >= span.0 && *s <= span.1)
        .map(|s| s - span.0)
}

/// An unwrapped span, kept inside the path when the path is open; a loop's
/// spans may wrap.
fn clamp_span(path: &CenterlinePath, start: f32, end: f32) -> (f32, f32) {
    if path.is_closed() {
        (start, end)
    } else {
        let total = path.total_length_m();
        (start.clamp(0.0, total), end.clamp(0.0, total))
    }
}

/// Segments of a path: one per sample, the last closing the loop on a
/// closed path; one fewer on an open path.
fn segment_count(path: &CenterlinePath) -> usize {
    let n = path.samples().len();
    if path.is_closed() {
        n
    } else {
        n.saturating_sub(1)
    }
}

/// Blend a height from the road edge into the terrain with distance — the
/// single-road profile of [`TerrainHeightfield::ground_height_at`], kept
/// as a plain function for callers that already know both heights.
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
            // The ground itself too, on both roads' account.
            assert!(f.ground_height_at(x, 0.0) < -0.05);
            assert!(f.ground_height_at(x, 30.0) < 24.95);
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

    /// The verge: right beside the road the ground is the road edge less
    /// [`VERGE_DROP_M`], everywhere, and it eases into the terrain by
    /// [`BLEND_END_M`] out.
    #[test]
    fn ground_hugs_the_road_edge_then_blends_to_terrain() {
        let path = CenterlinePath::from_track(&hilly_loop()).unwrap();
        let f = TerrainHeightfield::from_path(&path).unwrap();
        // 200 m into the low leg, measuring out from its right edge. (The
        // leg is a spline through four nodes, so it bulges: probe relative
        // to the sampled cross-section, not to the node line.)
        let s = path.sample_at(200.0);
        let road_z = offset_point(&s, -s.width_right_m).2;
        let at = |beyond: f32| {
            let p = offset_point(&s, -(s.width_right_m + beyond));
            f.ground_height_at(p.0, p.1)
        };
        let terrain_far = {
            let p = offset_point(&s, -(s.width_right_m + 60.0));
            f.height_at(p.0, p.1)
        };

        for beyond in [0.0, 1.0, 3.0, 5.0, 6.0] {
            assert!(
                (at(beyond) - (road_z - VERGE_DROP_M)).abs() < 0.02,
                "{beyond} m out: {} vs verge {}",
                at(beyond),
                road_z - VERGE_DROP_M
            );
        }
        assert!(
            (at(60.0) - terrain_far).abs() < 0.02,
            "60 m out: {} vs terrain {terrain_far}",
            at(60.0)
        );
        // Monotone between, and no step: consecutive metres differ by less
        // than a tenth of the whole drop.
        let total = (at(6.0) - at(40.0)).abs().max(0.01);
        let mut prev = at(6.0);
        for beyond in 7..=40 {
            let z = at(beyond as f32);
            assert!(
                (z - prev).abs() <= total * 0.12 + 0.01,
                "step of {} m at {beyond} m out",
                z - prev
            );
            prev = z;
        }
    }

    /// The pit lane is a road too: the ground hugs it, and the surface
    /// query reports its deck inside it.
    #[test]
    fn pit_lane_is_treated_like_road() {
        let track = hilly_loop();
        let path = CenterlinePath::from_track(&track).unwrap();
        // A 10 m wide lane 20 m right of the centerline along the low leg.
        let lane_node = |station: f32| {
            let p = offset_point(&path.sample_at(station), -20.0);
            [p.0, p.1, p.2]
        };
        let lane = CenterlinePath::from_polyline(
            &[
                lane_node(100.0),
                lane_node(150.0),
                lane_node(200.0),
                lane_node(250.0),
                lane_node(300.0),
            ],
            5.0,
        )
        .unwrap();
        let f = TerrainHeightfield::from_paths(&path, &[&lane]).unwrap();
        let s = path.sample_at(200.0);
        let probe = |lat: f32| offset_point(&s, lat);
        let verge = probe(-s.width_right_m).2 - VERGE_DROP_M;
        // 8 m from the track edge, a lone-road blend would already be
        // leaving the verge; with the lane 2 m further out the ground
        // stays put.
        let p = probe(-13.0);
        assert!(
            (f.ground_height_at(p.0, p.1) - verge).abs() < 0.02,
            "{} vs {verge}",
            f.ground_height_at(p.0, p.1)
        );
        // Inside the lane, the surface is the lane deck, not the verge.
        let p = probe(-20.0);
        assert!((f.surface_height_at(p.0, p.1) - p.2).abs() < 0.02);
        // Beside the lane's far edge, still the verge.
        let p = probe(-28.0);
        assert!((f.ground_height_at(p.0, p.1) - verge).abs() < 0.02);
    }

    /// The surface query: road (with banking) on the road, ground off it.
    #[test]
    fn surface_is_the_banked_road_inside_and_the_verge_outside() {
        let mut track = hilly_loop();
        for n in &mut track.nodes {
            n.banking = Some(0.2);
        }
        let path = CenterlinePath::from_track(&track).unwrap();
        let f = TerrainHeightfield::from_path(&path).unwrap();
        let s = path.sample_at(200.0);
        let expect = |lat: f32| offset_point(&s, lat).2;
        let at = |lat: f32| {
            let p = offset_point(&s, lat);
            f.surface_height_at(p.0, p.1)
        };
        let on = at(3.0);
        assert!((on - expect(3.0)).abs() < 0.05, "{on} vs {}", expect(3.0));
        let off = at(7.0);
        assert!(
            (off - (expect(5.0) - VERGE_DROP_M)).abs() < 0.05,
            "{off} vs {}",
            expect(5.0) - VERGE_DROP_M
        );
        // The ground never rises through the low edge of the banked road.
        for lat in [-4.9f32, -3.0, 0.0, 3.0, 4.9] {
            let (x, y, _) = offset_point(&s, lat);
            assert!(f.ground_height_at(x, y) < expect(lat) - 0.05);
        }
    }

    #[test]
    fn sidecar_covers_the_road_with_its_margin() {
        let f = field();
        let g = f.bake_ground_sidecar(GROUND_SIDECAR_CELL_M);
        assert_eq!(g.version, GROUND_SIDECAR_VERSION);
        assert_eq!(g.heights_cm.len(), (g.cols * g.rows) as usize);
        assert!(g.origin_x <= -5.0 - GROUND_SIDECAR_MARGIN_M + 0.01);
        let max_x = g.origin_x + (g.cols - 1) as f32 * g.cell_m;
        assert!(max_x >= 400.0 + 5.0 + GROUND_SIDECAR_MARGIN_M - 0.01);
        // A sample on the low straight's centerline reads the road.
        let col = ((200.0 - g.origin_x) / g.cell_m).round() as u32;
        let row = ((0.0 - g.origin_y) / g.cell_m).round() as u32;
        let x = g.origin_x + col as f32 * g.cell_m;
        let y = g.origin_y + row as f32 * g.cell_m;
        assert!((g.height_m(col, row) - f.surface_height_at(x, y)).abs() < 0.011);
    }
}
