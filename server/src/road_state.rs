//! The road through a session: rubber, marbles and water, laid and taken
//! away by the cars and the weather.
//!
//! The track file's grip is one figure per centerline sample, the same for
//! the whole session. Real asphalt is not: a line rubbers in as the field
//! laps it, the rubber the tyres shed off that line collects as marbles
//! where nobody drives, rain stands in the dips and the cars wipe a line
//! dry, and a shower washes the rubber away again. [`RoadState`] holds all
//! of it on every session's track: the lap cut into cells of [`CELL_M`],
//! each cut across the road into bins of [`BIN_M`] (from [`HALF_SPAN_M`]
//! right of the centerline to as far left, held to the road's own width),
//! and every wheel that crosses into a cell lays its pass into the bin it
//! runs in. Per cell:
//!
//! - **water**: a depth, in the weather's units (1 is heavy rain on a flat
//!   road), that the rain feeds toward [`RoadState::rain`] ([`RAIN_RATE`])
//!   and drainage and the sun take away ([`DRAIN_RATE`]); a **low** cell
//!   (the road a step below its surroundings along the lap, read off the
//!   centerline's elevation) drains slowly and holds [`PUDDLE_GAIN`] more,
//!   so a dip stands in water deeper than the rain leaves on the flat
//!   (`water` over 1 is a puddle, which costs every tyre:
//!   `tyre_thermal::Compound::grip_on`);
//!
//! and per bin:
//!
//! - **dry**, 0..1: what the wheels have wiped of the cell's water there
//!   ([`DRY_PER_PASS`]), wetted again by the rain ([`REWET_RATE`]); in
//!   steady rain no bin gets drier than [`line_dry_ceiling`]. A car on a
//!   line of its own dries its own line.
//! - **rubber**, 0..1: laid by every dry pass ([`RUBBER_PER_PASS`]), washed
//!   off by rain ([`RUBBER_WASH`]). A session starts with the raceline
//!   rubbered to the host's level (`SessionConditions::track_rubber`, half
//!   way by default) across [`START_LINE_HALF_M`] either side of it, and
//!   the road beyond at that level or [`RUBBER_REFERENCE`], whichever is
//!   less; the reference is the level every car was calibrated on, so a
//!   default start grips exactly as the track file says everywhere. Rubber grips ([`RUBBER_GRIP`] a unit) in the dry and
//!   is greasy in the wet ([`WET_RUBBER_GRIP`]).
//! - **marbles**, 0..1: every car's pass sheds a little over the whole
//!   width of the road ([`MARBLES_PER_PASS`]), every wheel sweeps its own
//!   bin clean ([`MARBLE_SWEEP`]), so they pile up off the line; the rain
//!   washes them off. Marbles cost up to [`MARBLE_GRIP`] of the grip.
//!
//! [`RoadState::sample`] is what a tyre finds under it: the water and what
//! the road grips against the road the session was baked for. The physics
//! reads it per wheel and tells the AI through `CarState::surface_grip_share`.
//! Everything is a function of the cars' positions and the session clock,
//! visited in the participants' order, so the sim stays deterministic; the
//! state steps every [`STEP_TICKS`] ticks.

use serde::{Deserialize, Serialize};

use crate::data::TrackConfig;

/// The lap's cells, m.
pub const CELL_M: f32 = 10.0;
/// The bins across a cell, m.
pub const BIN_M: f32 = 1.0;
/// How far either side of the centerline the bins reach at most, m.
pub const HALF_SPAN_M: f32 = 16.0;
/// Bins across a cell.
pub const BINS: usize = (2.0 * HALF_SPAN_M / BIN_M) as usize;
/// The state steps every so many ticks: everything in it is slow.
pub const STEP_TICKS: u32 = 7;

/// Share of the gap to the rain's standing depth the rain closes per second.
pub const RAIN_RATE: f32 = 0.01;
/// Share of the depth over the standing depth that drains per second on a
/// flat road under a mild sky; the sun and a warm track speed it up
/// ([`RoadState::step`]'s `evaporation`).
pub const DRAIN_RATE: f32 = 0.004;
/// How much deeper a fully low cell stands than the rain's figure.
pub const PUDDLE_GAIN: f32 = 0.5;
/// A cell this far below its surroundings along the lap, m, is fully low:
/// a real compression (a dip of a metre and a half over a hundred metres
/// either way), not the gentle fall of a straight.
pub const LOW_DEPTH_M: f32 = 1.5;
/// How far either way the surroundings are read, m.
const LOW_REACH_M: f32 = 100.0;
/// What one wheel's pass dries of the water in its bin, and how fast the
/// rain wets a dried bin again per second, as a share of what is dry (in
/// heavy rain a wiped line is wet again in a few minutes without traffic;
/// a field of twenty cars a lap keeps it a good deal drier than the rest).
pub const DRY_PER_PASS: f32 = 0.05;
pub const REWET_RATE: f32 = 0.004;
/// A wheel's pass reaches this share of its effect into the bins either
/// side (a tyre is a third of a metre wide and no two laps are alike).
const NEIGHBOUR_SHARE: f32 = 0.3;

/// What one dry wheel pass adds of the rubber still to lay. A field of
/// twenty cars over twenty laps takes the line from the default half way
/// to some 85%.
pub const RUBBER_PER_PASS: f32 = 0.004;
/// Share of the rubber heavy rain washes off per second (a heavy shower of
/// ten minutes leaves under half of it).
pub const RUBBER_WASH: f32 = 0.0015;
/// The rubber every car.toml was calibrated on: the line grips exactly as
/// filed there.
pub const RUBBER_REFERENCE: f32 = 0.5;
/// Grip per unit of rubber over the reference in the dry: a green line is
/// 1.5% slower, a fully rubbered one 1.5% quicker.
pub const RUBBER_GRIP: f32 = 0.03;
/// Grip lost per unit of rubber over the reference once the road is wet:
/// rubber under a film of water is greasy.
pub const WET_RUBBER_GRIP: f32 = 0.06;
/// The start's rubber covers this far either side of the raceline, m, in
/// full, and falls away over [`START_LINE_FADE_M`] beyond.
pub const START_LINE_HALF_M: f32 = 2.0;
const START_LINE_FADE_M: f32 = 1.5;

/// What one car's pass sheds as marbles into every bin of the cell.
pub const MARBLES_PER_PASS: f32 = 0.0015;
/// Share of the marbles in its bin one wheel's pass sweeps away.
pub const MARBLE_SWEEP: f32 = 0.15;
/// Share of the marbles heavy rain washes off per second.
pub const MARBLE_WASH: f32 = 0.004;
/// Grip lost on a bin full of marbles.
pub const MARBLE_GRIP: f32 = 0.15;

/// The road's grip for a given amount of water, against the dry road:
/// the weather's own figures (`Weather::road_grip_factor`: 0.86 in light
/// rain at 0.5, 0.74 in heavy at 1.0), linear between, and on down into
/// a puddle.
pub fn road_grip_for(water: f32) -> f32 {
    let w = water.max(0.0);
    if w <= 0.5 {
        1.0 - (1.0 - 0.86) * (w / 0.5)
    } else if w <= 1.0 {
        0.86 - (0.86 - 0.74) * ((w - 0.5) / 0.5)
    } else {
        (0.74 - 0.12 * (w - 1.0)).max(0.5)
    }
}

/// Painted curbs on top of [`road_grip_for`] (`Weather::curb_grip_factor`:
/// 0.85 in light rain, 0.75 in heavy).
pub fn curb_grip_for(water: f32) -> f32 {
    let w = water.clamp(0.0, 1.0);
    if w <= 0.5 {
        1.0 - 0.15 * (w / 0.5)
    } else {
        0.85 - 0.10 * ((w - 0.5) / 0.5)
    }
}

/// Wet grass and gravel on top of [`road_grip_for`]
/// (`Weather::off_track_grip_factor`: 0.85 in light rain, 0.7 in heavy).
pub fn off_track_grip_for(water: f32) -> f32 {
    let w = water.clamp(0.0, 1.0);
    if w <= 0.5 {
        1.0 - 0.15 * (w / 0.5)
    } else {
        0.85 - 0.15 * ((w - 0.5) / 0.5)
    }
}

/// The driest a bin gets under steady rain of `rain`: the cars wipe it,
/// the rain wets it; nothing dries fully but a track the rain has left.
pub fn line_dry_ceiling(rain: f32) -> f32 {
    (1.0 - rain.clamp(0.0, 1.0) * 0.8).max(0.0)
}

/// The grip the rubber gives, against the reference, on a road with
/// `water` on it.
pub fn rubber_grip(rubber: f32, water: f32) -> f32 {
    let wet = (water / 0.5).clamp(0.0, 1.0);
    let per_unit = RUBBER_GRIP - wet * (RUBBER_GRIP + WET_RUBBER_GRIP);
    1.0 + (rubber - RUBBER_REFERENCE) * per_unit
}

/// What a tyre finds on the road under it.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct RoadSample {
    /// Water on the road there, in the weather's units.
    pub water: f32,
    /// The paved road's grip there against the road the session was baked
    /// at: the water against the baked water, the rubber, the marbles.
    pub grip: f32,
    /// The water's share of it alone: what asphalt nobody races on (the
    /// run-off, the pit lane) grips against the baked road.
    pub wet_grip: f32,
    /// The grass beside the road against the grass the session was baked
    /// at (the water alone).
    pub off_grip: f32,
    /// A painted curb's further share against the baked one.
    pub curb_grip: f32,
}

impl RoadSample {
    /// A road exactly as baked.
    pub const BAKED: RoadSample = RoadSample {
        water: 0.0,
        grip: 1.0,
        wet_grip: 1.0,
        off_grip: 1.0,
        curb_grip: 1.0,
    };
}

/// One cell of the lap.
#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
pub struct Cell {
    /// Water, in the weather's units (1 is heavy rain on a flat road).
    pub depth: f32,
    /// How low the road lies here against its surroundings, 0..1.
    pub low: f32,
    /// The raceline's lateral offset at the cell, m, positive right.
    pub line_lateral_m: f32,
    /// The centerline at the cell's middle, and the unit vector to its left.
    pub x: f32,
    pub y: f32,
    pub left_x: f32,
    pub left_y: f32,
    /// The bins the road covers, `first..=last` (the rest stay empty).
    pub first: u8,
    pub last: u8,
    /// Cars that crossed into the cell since the state last stepped.
    passes: u16,
    /// What they shed as marbles, in passes' worth.
    shed: f32,
}

/// The road along the lap.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct RoadState {
    /// The rain falling now, in the weather's units: what the water feeds
    /// toward. Set by the session from its live conditions.
    pub rain: f32,
    /// The water the session's road grip was baked at
    /// (`TrackSurface::baked_water`): every grip figure is against it.
    pub baked_water: f32,
    pub lap_m: f32,
    pub cells: Vec<Cell>,
    /// Per bin, [`BINS`] to a cell, cell after cell.
    pub dry: Vec<f32>,
    pub rubber: Vec<f32>,
    pub marbles: Vec<f32>,
    /// Wheel passes per bin since the last step.
    #[serde(skip)]
    wheel_passes: Vec<u16>,
}

impl RoadState {
    /// The road for `track` under its baked weather
    /// (`track_surface.water`), already standing: the rain has been
    /// falling a while, so the flat road holds the weather's figure and the
    /// dips their puddles, no line is dry yet, and the raceline carries
    /// `rubber` (0 green to 1 fully rubbered; [`RUBBER_REFERENCE`] is what
    /// the cars were calibrated on).
    pub fn new(track: &TrackConfig, rubber: f32) -> Self {
        let rain = track.track_surface.water.max(0.0);
        let lap_m = crate::laps::track_length_m(track).max(CELL_M);
        let n = (lap_m / CELL_M).ceil().max(1.0) as usize;
        let mut cells = vec![Cell::default(); n];
        let mut zs = Vec::with_capacity(n);
        let mut hint = None;
        for (i, cell) in cells.iter_mut().enumerate() {
            let station = (i as f32 + 0.5) * CELL_M;
            if track.centerline.is_empty() {
                zs.push(0.0);
                cell.first = 0;
                cell.last = (BINS - 1) as u8;
                continue;
            }
            let (x, y, z, heading) = crate::physics::pose_at_station(&track.centerline, station);
            zs.push(z);
            cell.x = x;
            cell.y = y;
            cell.left_x = -heading.sin();
            cell.left_y = heading.cos();
            let (left, right) = widths_at(track, station);
            // Lateral is positive right: the right edge is at +right.
            cell.first = bin_of(-left).unwrap_or(0) as u8;
            cell.last = bin_of(right).unwrap_or(BINS - 1) as u8;
            let (lateral, found) = raceline_lateral_at(track, x, y, heading, hint);
            cell.line_lateral_m = lateral;
            hint = found;
        }
        // How low each cell lies: the road's elevation against the mean of
        // its surroundings along the lap.
        let reach_cells = (LOW_REACH_M / CELL_M).round().max(1.0) as i64;
        for i in 0..n {
            let mut sum = 0.0;
            let mut count = 0.0f32;
            for k in -reach_cells..=reach_cells {
                let j = (i as i64 + k).rem_euclid(n as i64) as usize;
                sum += zs[j];
                count += 1.0;
            }
            let mean = sum / count.max(1.0);
            cells[i].low = ((mean - zs[i]) / LOW_DEPTH_M).clamp(0.0, 1.0);
            cells[i].depth = Self::standing_depth(rain, cells[i].low);
        }
        let mut rubber_bins = vec![0.0; n * BINS];
        // The line carries the host's level; the road off it no more than
        // the reference, so a default start is the road every car was
        // calibrated on from edge to edge, and only a rubbered start shows
        // the line against the rest.
        let rubber = rubber.clamp(0.0, 1.0);
        let off_line = rubber.min(RUBBER_REFERENCE);
        for (i, cell) in cells.iter().enumerate() {
            for b in cell.first as usize..=cell.last as usize {
                let off = (bin_centre(b) - cell.line_lateral_m).abs();
                let fade = ((off - START_LINE_HALF_M).max(0.0) / START_LINE_FADE_M).powi(2);
                let on_line = (-2.0 * fade).exp();
                rubber_bins[i * BINS + b] = off_line + (rubber - off_line) * on_line;
            }
        }
        Self {
            rain,
            baked_water: track.track_surface.baked_water,
            lap_m,
            cells,
            dry: vec![0.0; n * BINS],
            rubber: rubber_bins,
            marbles: vec![0.0; n * BINS],
            wheel_passes: vec![0; n * BINS],
        }
    }

    /// The depth a cell `low` settles at under `rain`.
    fn standing_depth(rain: f32, low: f32) -> f32 {
        rain * (1.0 + PUDDLE_GAIN * low)
    }

    pub fn cell_index(&self, station_m: f32) -> usize {
        let n = self.cells.len().max(1);
        ((station_m.rem_euclid(self.lap_m.max(1.0)) / CELL_M) as usize).min(n - 1)
    }

    /// `lateral_m` (positive right) as a position across the bins: the
    /// lower bin and the share of the next, held to the cell's road.
    fn across(cell: &Cell, lateral_m: f32) -> (usize, usize, f32) {
        let pos = (lateral_m + HALF_SPAN_M) / BIN_M - 0.5;
        let (first, last) = (cell.first as f32, cell.last as f32);
        let pos = pos.clamp(first, last.max(first));
        let lo = pos.floor() as usize;
        let hi = (lo + 1).min(cell.last as usize);
        (lo, hi, pos - lo as f32)
    }

    fn bin_value(values: &[f32], base: usize, across: (usize, usize, f32)) -> f32 {
        let (lo, hi, t) = across;
        values[base + lo] * (1.0 - t) + values[base + hi] * t
    }

    /// The water under a tyre at `station_m` along the lap, `lateral_m`
    /// from the centerline (positive right).
    pub fn water_at(&self, station_m: f32, lateral_m: f32) -> f32 {
        self.sample(station_m, lateral_m).water
    }

    /// What a tyre at `station_m`, `lateral_m` from the centerline
    /// (positive right), finds on the road.
    pub fn sample(&self, station_m: f32, lateral_m: f32) -> RoadSample {
        if self.cells.is_empty() {
            return RoadSample::BAKED;
        }
        let i = self.cell_index(station_m);
        let cell = &self.cells[i];
        let base = i * BINS;
        let across = Self::across(cell, lateral_m);
        let dry = Self::bin_value(&self.dry, base, across);
        let water = (cell.depth * (1.0 - dry)).max(0.0);
        let rubber = Self::bin_value(&self.rubber, base, across);
        let marbles = Self::bin_value(&self.marbles, base, across);
        let baked = self.baked_water;
        let wet = road_grip_for(water) / road_grip_for(baked).max(1e-3);
        let grip = wet * rubber_grip(rubber, water) * (1.0 - MARBLE_GRIP * marbles.clamp(0.0, 1.0));
        // The grass holds the cell's water: nobody dries it.
        let off_grip = road_grip_for(cell.depth) * off_track_grip_for(cell.depth)
            / (road_grip_for(baked) * off_track_grip_for(baked)).max(1e-3);
        RoadSample {
            water,
            grip,
            wet_grip: wet,
            off_grip,
            curb_grip: curb_grip_for(water) / curb_grip_for(baked).max(1e-3),
        }
    }

    /// The puddle's share of a cell, 0..1 (for a display, or a test).
    pub fn puddle_at(&self, station_m: f32) -> f32 {
        if self.cells.is_empty() {
            return 0.0;
        }
        self.cells[self.cell_index(station_m)].low
    }

    /// How many cells hold a real puddle (over half low).
    pub fn puddle_cells(&self) -> usize {
        self.cells.iter().filter(|c| c.low > 0.5).count()
    }

    /// The water across the lap, the cells' mean depth (what a car on the
    /// road meets, apart from the dried lines): the tyre the track calls for.
    pub fn mean_water(&self) -> f32 {
        if self.cells.is_empty() {
            return self.rain;
        }
        self.cells.iter().map(|c| c.depth).sum::<f32>() / self.cells.len() as f32
    }

    /// The water on the raceline across the lap, the cells' mean at the
    /// line: what a driver deciding on tyres looks at.
    pub fn mean_line_water(&self) -> f32 {
        if self.cells.is_empty() {
            return self.rain;
        }
        let sum: f32 = self
            .cells
            .iter()
            .enumerate()
            .map(|(i, c)| {
                c.depth
                    * (1.0
                        - Self::bin_value(&self.dry, i * BINS, Self::across(c, c.line_lateral_m)))
            })
            .sum();
        sum / self.cells.len() as f32
    }

    /// The rubber on the raceline across the lap, the cells' mean at the
    /// line, 0..1.
    pub fn mean_line_rubber(&self) -> f32 {
        if self.cells.is_empty() {
            return RUBBER_REFERENCE;
        }
        let sum: f32 = self
            .cells
            .iter()
            .enumerate()
            .map(|(i, c)| {
                Self::bin_value(&self.rubber, i * BINS, Self::across(c, c.line_lateral_m))
            })
            .sum();
        sum / self.cells.len() as f32
    }

    /// Lateral offset of `(x, y)` from the centerline at the cell holding
    /// `station_m`, m, positive right.
    pub fn lateral_of(&self, station_m: f32, x: f32, y: f32) -> f32 {
        if self.cells.is_empty() {
            return 0.0;
        }
        let c = &self.cells[self.cell_index(station_m)];
        -((x - c.x) * c.left_x + (y - c.y) * c.left_y)
    }

    /// A car crossed into the cell at `station_m` with its wheels running
    /// at the `wheel_laterals` (m, positive right): each lays its pass in
    /// its bin, and the car sheds its marbles over the cell, `shed` times a
    /// pass's share (more the harder it corners: [`shed_for_lateral_g`]).
    pub fn note_pass(&mut self, station_m: f32, wheel_laterals: &[f32], shed: f32) {
        if self.cells.is_empty() {
            return;
        }
        let i = self.cell_index(station_m);
        let cell = &mut self.cells[i];
        cell.passes = cell.passes.saturating_add(1);
        cell.shed += shed.max(0.0);
        for &lateral in wheel_laterals {
            if let Some(b) = bin_of(lateral) {
                let slot = &mut self.wheel_passes[i * BINS + b];
                *slot = slot.saturating_add(1);
            }
        }
    }

    /// Advance the road by `dt` seconds (the ticks since the last step):
    /// the rain, the drainage (`evaporation` scales it: 1 under a mild
    /// sky, more under a high sun on warm asphalt), the passes' drying,
    /// rubber and marbles, and the rain's rewetting and washing.
    pub fn step(&mut self, dt: f32, evaporation: f32) {
        let rain = self.rain.max(0.0);
        let ceiling = line_dry_ceiling(rain);
        let drain = DRAIN_RATE * evaporation.max(0.0);
        for (i, cell) in self.cells.iter_mut().enumerate() {
            let target = Self::standing_depth(rain, cell.low);
            // The rain fills toward the standing depth; the drainage takes
            // what stands above it (everything, once the rain has gone),
            // slowly in a dip. In steady rain a cell holds its standing
            // depth exactly, which is what the road's grip was baked for.
            if cell.depth < target {
                cell.depth += (target - cell.depth) * (RAIN_RATE * dt).min(1.0);
            }
            let excess = (cell.depth - target).max(0.0);
            cell.depth -= excess * (drain * (1.0 - 0.7 * cell.low) * dt).min(1.0);
            cell.depth = cell.depth.max(0.0);
            let wet = (cell.depth / 0.5).clamp(0.0, 1.0);
            cell.passes = 0;
            let car_passes = std::mem::take(&mut cell.shed);
            let base = i * BINS;
            let (first, last) = (cell.first as usize, cell.last as usize);
            for b in first..=last {
                let k = base + b;
                // A wheel in this bin counts in full, one in the next bin
                // either way in part.
                let own = self.wheel_passes[k] as f32;
                let near = if b > first {
                    self.wheel_passes[k - 1] as f32
                } else {
                    0.0
                } + if b < last {
                    self.wheel_passes[k + 1] as f32
                } else {
                    0.0
                };
                let passes = own + NEIGHBOUR_SHARE * near;
                // The passes wipe the water; the rain wets it again.
                let dry = &mut self.dry[k];
                *dry += (1.0 - *dry) * (DRY_PER_PASS * passes).min(1.0);
                *dry -= *dry * REWET_RATE * rain * dt;
                *dry = dry.clamp(0.0, ceiling);
                // Dry passes lay rubber; rain washes it.
                let rubber = &mut self.rubber[k];
                *rubber += (1.0 - *rubber) * (RUBBER_PER_PASS * passes * (1.0 - wet)).min(1.0);
                *rubber -= *rubber * (RUBBER_WASH * rain * dt).min(1.0);
                *rubber = rubber.clamp(0.0, 1.0);
                // Every car sheds over the cell; every wheel sweeps its bin.
                let marbles = &mut self.marbles[k];
                *marbles += MARBLES_PER_PASS * car_passes * (1.0 - wet);
                *marbles *= (1.0 - MARBLE_SWEEP).powf(passes);
                *marbles -= *marbles * (MARBLE_WASH * rain * dt).min(1.0);
                *marbles = marbles.clamp(0.0, 1.0);
            }
            for b in first..=last {
                self.wheel_passes[base + b] = 0;
            }
        }
    }
}

/// How much a car cornering at `lateral_g` sheds as marbles, in passes'
/// worth: a tyre working hard sheds rubber, one rolling straight almost
/// none, so the marbles gather beside the line through the corners.
pub fn shed_for_lateral_g(lateral_g: f32) -> f32 {
    (0.1 + lateral_g.abs() / 1.5).min(2.0)
}

/// The bin `lateral_m` (positive right) falls in, if it is within the span.
fn bin_of(lateral_m: f32) -> Option<usize> {
    let b = ((lateral_m + HALF_SPAN_M) / BIN_M).floor();
    (b >= 0.0 && (b as usize) < BINS).then_some(b as usize)
}

/// The middle of bin `b`, m right of the centerline.
fn bin_centre(b: usize) -> f32 {
    (b as f32 + 0.5) * BIN_M - HALF_SPAN_M
}

/// The road's width left and right of the centerline at `station_m`.
fn widths_at(track: &TrackConfig, station_m: f32) -> (f32, f32) {
    let cl = &track.centerline;
    let i = cl
        .partition_point(|p| p.distance_from_start_m < station_m)
        .min(cl.len() - 1);
    (cl[i].width_left_m.max(1.0), cl[i].width_right_m.max(1.0))
}

/// The raceline's lateral offset from the centerline point `(cx, cy)`
/// heading `heading`, m, positive right; 0 for a track without a raceline.
/// `hint` is the raceline index found for the cell before, and the search
/// starts from it.
fn raceline_lateral_at(
    track: &TrackConfig,
    cx: f32,
    cy: f32,
    heading: f32,
    hint: Option<usize>,
) -> (f32, Option<usize>) {
    let line = &track.raceline;
    if line.is_empty() {
        return (0.0, None);
    }
    let d2 = |j: usize| (line[j].x - cx).powi(2) + (line[j].y - cy).powi(2);
    let n = line.len();
    let best = match hint {
        // Walk forward from the last cell's point while it gets closer,
        // over a window that covers the gap between two cells.
        Some(h) => {
            let mut best = (d2(h), h);
            for k in 1..64 {
                let j = (h + k) % n;
                let d = d2(j);
                if d < best.0 {
                    best = (d, j);
                }
            }
            // Lost (a raceline sampled very differently): search it all.
            if best.0 > 400.0 {
                (0..n)
                    .map(|j| (d2(j), j))
                    .fold(best, |a, b| if b.0 < a.0 { b } else { a })
            } else {
                best
            }
        }
        None => (0..n)
            .map(|j| (d2(j), j))
            .fold((f32::MAX, 0), |a, b| if b.0 < a.0 { b } else { a }),
    };
    let p = &line[best.1];
    let left = (p.x - cx) * -heading.sin() + (p.y - cy) * heading.cos();
    ((-left).clamp(-15.0, 15.0), Some(best.1))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::data::{RacelinePoint, TrackPoint};

    /// A 2 km straight 16 m wide with a compression 3 m deep around 1000 m
    /// and a raceline 2 m right of the centerline.
    fn dipped_track(rain: f32) -> TrackConfig {
        let mut track = TrackConfig {
            centerline: (0..500)
                .map(|i| {
                    let s = i as f32 * 4.0;
                    let dip = (-((s - 1000.0) / 40.0).powi(2)).exp();
                    TrackPoint {
                        x: s,
                        z: 10.0 - 3.0 * dip,
                        distance_from_start_m: s,
                        width_left_m: 8.0,
                        width_right_m: 8.0,
                        ..Default::default()
                    }
                })
                .collect(),
            ..TrackConfig::default()
        };
        track.raceline = (0..500)
            .map(|i| RacelinePoint {
                x: i as f32 * 4.0,
                y: -2.0,
                z: 10.0,
            })
            .collect();
        track.track_surface.water = rain;
        track.track_surface.baked_water = rain;
        track
    }

    /// `laps` passes of a car along `lateral` (its wheels 0.8 m either
    /// side) through every cell, stepping between them over `seconds`.
    fn drive(road: &mut RoadState, lateral: f32, laps: usize, seconds: f32) {
        let n = road.cells.len();
        let dt = seconds / laps.max(1) as f32;
        for _ in 0..laps {
            for i in 0..n {
                road.note_pass(
                    i as f32 * CELL_M + 1.0,
                    &[lateral - 0.8, lateral + 0.8],
                    1.0,
                );
            }
            road.step(dt, 1.0);
        }
    }

    #[test]
    fn the_grip_figures_follow_the_weathers() {
        assert_eq!(road_grip_for(0.0), 1.0);
        assert!((road_grip_for(0.5) - 0.86).abs() < 1e-6);
        assert!((road_grip_for(1.0) - 0.74).abs() < 1e-6);
        assert!(road_grip_for(1.5) < 0.74 && road_grip_for(10.0) >= 0.5);
        assert!(
            (curb_grip_for(0.5) - 0.85).abs() < 1e-6 && (curb_grip_for(1.0) - 0.75).abs() < 1e-6
        );
        assert!((off_track_grip_for(1.0) - 0.7).abs() < 1e-6);
        assert!(line_dry_ceiling(0.0) == 1.0 && line_dry_ceiling(1.0) < 0.3);
        assert_eq!(rubber_grip(RUBBER_REFERENCE, 0.0), 1.0);
        assert!(rubber_grip(1.0, 0.0) > 1.01 && rubber_grip(0.0, 0.0) < 0.99);
        assert!(rubber_grip(1.0, 1.0) < 1.0, "rubber is greasy in the wet");
    }

    #[test]
    fn a_default_start_grips_as_filed_everywhere_and_a_rubbered_one_shows_its_line() {
        let road = RoadState::new(&dipped_track(0.0), RUBBER_REFERENCE);
        // The raceline is 2 m right; the wheels run 0.8 m either side.
        for lateral in [1.2, 2.0, 2.8] {
            let s = road.sample(500.0, lateral);
            assert_eq!(s.grip, 1.0, "on the line at {lateral}");
            assert_eq!(s.water, 0.0);
        }
        assert_eq!(road.sample(500.0, -5.0).grip, 1.0, "off the line too");
        assert!((road.mean_line_rubber() - RUBBER_REFERENCE).abs() < 1e-3);
        let rubbered = RoadState::new(&dipped_track(0.0), 1.0);
        assert!(rubbered.sample(500.0, 2.0).grip > 1.01);
        assert_eq!(rubbered.sample(500.0, -5.0).grip, 1.0);
        let green = RoadState::new(&dipped_track(0.0), 0.0);
        assert!(green.sample(500.0, 2.0).grip < 0.99);
        assert_eq!(green.sample(500.0, 2.0).grip, green.sample(500.0, -5.0).grip);
    }

    #[test]
    fn the_line_rubbers_in_and_marbles_pile_up_beside_it() {
        let mut road = RoadState::new(&dipped_track(0.0), RUBBER_REFERENCE);
        // Twenty cars, twenty laps.
        drive(&mut road, 2.0, 400, 3600.0);
        let on = road.sample(500.0, 2.0 + 0.8).grip;
        let beside = road.sample(500.0, 2.0 + 4.0).grip;
        println!("after 400 passes: line {on:.4}, 4 m off it {beside:.4}");
        assert!(on > 1.008, "the line rubbers in: {on}");
        assert!(beside < 0.95, "marbles off the line: {beside}");
        let i = road.cell_index(500.0);
        let k = i * BINS + bin_of(2.8).unwrap();
        assert!(road.marbles[k] < 0.05, "the wheels sweep their own path");
        // Off the road's width nothing is tracked: the bins stop at the edge.
        assert_eq!(road.sample(500.0, 15.0), road.sample(500.0, 9.0));
    }

    #[test]
    fn a_car_on_a_line_of_its_own_dries_its_own_line() {
        let mut road = RoadState::new(&dipped_track(0.5), RUBBER_REFERENCE);
        let before = road.water_at(400.0, -4.0);
        drive(&mut road, -4.0, 40, 180.0);
        let own = road.water_at(400.0, -4.0);
        let raceline = road.water_at(400.0, 2.0);
        println!("light rain: own line {own:.3}, the raceline {raceline:.3}, was {before:.3}");
        assert!(own < before * 0.8, "its line dries");
        assert!(raceline > own + 0.1, "the raceline nobody drove is wet");
        assert!(own > 0.05, "in steady rain no line dries fully");
    }

    #[test]
    fn a_dip_holds_a_puddle_and_the_flat_holds_the_rain() {
        let road = RoadState::new(&dipped_track(0.5), RUBBER_REFERENCE);
        let flat = road.water_at(200.0, 0.0);
        let dip = road.water_at(1000.0, 0.0);
        assert!((flat - 0.5).abs() < 0.02, "{flat}");
        assert!(dip > 0.6, "a puddle: {dip}");
        assert!(road.puddle_at(1000.0) > 0.5 && road.puddle_at(200.0) < 0.05);
        // The baked water is the reference: the flat grips as baked.
        assert!((road.sample(200.0, 0.0).grip / rubber_grip(0.0, 0.5) - 1.0).abs() < 0.03);
        // Off the lap's end it wraps.
        assert!((road.water_at(2000.0 + 200.0, 0.0) - flat).abs() < 0.03);
    }

    #[test]
    fn rain_arriving_wets_the_road_and_washes_the_rubber_and_the_track_dries_after() {
        let mut road = RoadState::new(&dipped_track(0.0), 0.9);
        let rubber_before = road.mean_line_rubber();
        road.rain = 1.0;
        for _ in 0..600 {
            road.step(1.0, 1.0);
        }
        let wet = road.water_at(200.0, 0.0);
        assert!(wet > 0.9, "ten minutes of heavy rain: {wet}");
        assert!(road.sample(200.0, 0.0).grip < 0.8);
        let washed = road.mean_line_rubber();
        assert!(washed < rubber_before * 0.5, "{rubber_before} -> {washed}");
        // The rain stops; the cars and the sun dry the road, the line first.
        road.rain = 0.0;
        drive(&mut road, 2.0, 60, 600.0);
        let line = road.water_at(200.0, 2.8);
        let rest = road.water_at(200.0, -5.0);
        println!("ten minutes after: line {line:.3}, off it {rest:.3}");
        assert!(line < rest, "a drying line");
        assert!(rest < 0.3, "the road drains");
        assert!(road.mean_water() < wet);
    }
}
