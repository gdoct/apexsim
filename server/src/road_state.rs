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
//! and every wheel that crosses into a cell lays its pass into the bin its
//! contact patch runs in, by what the tyre did since the last cell
//! ([`RoadWork`], added up by the physics): its load makes the pass (a
//! lifted wheel lays nothing), its sliding lays the rubber, the tread its
//! car wore is shed as marbles. Per cell:
//!
//! - **water**: a depth, in the weather's units (1 is heavy rain on a flat
//!   road), that the rain feeds toward [`RoadState::rain`] ([`RAIN_RATE`])
//!   and drainage and the sun take away ([`DRAIN_RATE`]); a **low** cell
//!   (the road a step below its surroundings along the lap, read off the
//!   centerline's elevation, or a banked or cambered road's low edge,
//!   [`edge_low`]) drains slowly and holds [`PUDDLE_GAIN`] more, so a dip
//!   or the inside of a cambered corner stands in water deeper than the
//!   rain leaves on the flat (`water` over 1 is a puddle, which costs every
//!   tyre: `tyre_thermal::Compound::grip_on`). The puddle lies across part
//!   of the road ([`RoadState::bin_depth`]): at the low edge of a banked
//!   road, at a seeded spot on a flat one, as wide as a dip is deep; the
//!   rest of the cell holds the flat road's water;
//!
//! and per bin:
//!
//! - **dry**, 0..1: what the wheels have wiped of the cell's water there
//!   ([`DRY_PER_PASS`]), wetted again by the rain ([`REWET_RATE`]); in
//!   steady rain no bin gets drier than [`line_dry_ceiling`]. A car on a
//!   line of its own dries its own line.
//! - **rubber**, 0..1: laid by dry passes by the tyre's sliding
//!   ([`RUBBER_PER_PASS`] for [`RUBBER_SLIDE_J`]), washed
//!   off by rain ([`RUBBER_WASH`]). A session starts with the raceline
//!   rubbered to the host's level (`SessionConditions::track_rubber`, half
//!   way by default) across [`START_LINE_HALF_M`] either side of it, and
//!   the road beyond at that level or [`RUBBER_REFERENCE`], whichever is
//!   less; the reference is the level every car was calibrated on, so a
//!   default start grips exactly as the track file says everywhere. Rubber grips ([`RUBBER_GRIP`] a unit) in the dry and
//!   is greasy in the wet ([`WET_RUBBER_GRIP`]).
//! - **marbles**, 0..1: every car sheds the tread its tyres wore over the
//!   whole width of the road ([`MARBLES_PER_PASS`] for [`MARBLE_WEAR_PCT`]),
//!   every wheel sweeps its own bin clean ([`MARBLE_SWEEP`]), so they pile
//!   up off the line; the rain washes them off. Marbles cost up to
//!   [`MARBLE_GRIP`] of the grip, and a tyre driven through them picks them
//!   up (`tyre_thermal`: `TireData::pickup`, a grip loss that is flung off
//!   over a lap).
//!
//! [`RoadState::sample`] is what a tyre finds under it: the water and what
//! the road grips against the road the session was baked for. The physics
//! reads it per wheel and tells the AI through `CarState::surface_grip_share`.
//! Snow is a fourth layer on the same bins. Under a snow sky ([`SNOWFALL_RATE`])
//! the road's **cover**, 0..1, builds toward 1 and the grip falls with it
//! ([`snow_grip`]: bare asphalt 1.0, covered 0.28, a film of it already most
//! of that); the baked road of a snow session is the covered one
//! (`Weather::Snow`'s 0.28), so everything is against it. Every wheel pass
//! packs its bin down toward [`SNOW_FLOOR`] (a driven line grips some 1.5
//! times the field's first lap, [`SNOW_CLEAR_PER_PASS`]);
//! the fall builds it again, a track over 0 °C melts it ([`SNOW_MELT_PER_C`]),
//! and the water on a bin the cars have cleared freezes when the track is
//! under [`FREEZE_C`]: **ice**, 0..1, that costs [`ICE_GRIP_LOSS`] of the grip
//! until the track thaws. The verge, which no wheel clears, keeps its own
//! cover per cell. Rubber and marbles lay on a snowy bin as little as on a
//! wet one.
//!
//! Everything is a function of the cars' positions and the session clock,
//! visited in the participants' order, so the sim stays deterministic; the
//! state steps every [`STEP_TICKS`] ticks.

use serde::{Deserialize, Serialize};

use crate::data::TrackConfig;

/// The lap's cells, m.
pub const CELL_M: f32 = 10.0;
/// The bins across a cell, m.
pub const BIN_M: f32 = 1.0;
/// A puddle's half-width across the road, m: the least, what a full dip
/// adds, and the seeded spread on top.
const PUDDLE_HALF_MIN_M: f32 = 1.5;
const PUDDLE_HALF_LOW_M: f32 = 3.0;
const PUDDLE_HALF_SPREAD_M: f32 = 1.5;
/// Banking past which the water runs to the low edge, rad (half a degree).
const PUDDLE_BANK_RAD: f32 = 0.009;
/// A banked or cambered road's low edge holds water as a cell this low
/// ([`RoadState::bin_depth`]'s puddle, at the edge) once the road leans
/// [`EDGE_LOW_MAX`] times [`EDGE_BANK_FULL_RAD`] past [`PUDDLE_BANK_RAD`]
/// (some 2 degrees): a stream along the kerb, never as deep as a real
/// compression.
pub const EDGE_LOW_MAX: f32 = 0.5;
const EDGE_BANK_FULL_RAD: f32 = 0.05;
/// The seeds of a puddle's place and width (`wind::hash01` salts).
const PUDDLE_SPOT_SALT: u64 = 0x5055_4444_4c45;
const PUDDLE_WIDTH_SALT: u64 = 0x5755_4444_4c45;

/// How far either side of the centerline the bins reach at most, m.
pub const HALF_SPAN_M: f32 = 16.0;
/// Bins across a cell.
pub const BINS: usize = (2.0 * HALF_SPAN_M / BIN_M) as usize;
/// The state steps every so many ticks: everything in it is slow.
pub const STEP_TICKS: u32 = 7;

/// Byte units for a cell's water depth on the wire ([`RoadState::write_rows`]):
/// 100 is the depth heavy rain leaves on a flat road (`Weather::water` 1.0),
/// so standing water reads over 100 and the full 0..2.55 range fits a byte.
pub const DEPTH_BYTE: f32 = 100.0;
/// Bytes of one cell on the wire: its water depth, then [`BINS`] bins of
/// rubber, marbles and dry.
pub const ROW_BYTES: usize = 1 + 3 * BINS;
/// Bytes of one cell's snow on the wire ([`RoadState::write_snow_rows`]): the
/// verge, then a bin's cover and ice.
pub const SNOW_ROW_BYTES: usize = 1 + 2 * BINS;
/// Bytes of one cell's geometry on the wire ([`RoadState::write_geometry`]).
pub const GEOMETRY_BYTES: usize = 16;
/// Cells sent in one round-robin `RoadState` message while racing: the
/// whole lap is refreshed over `cell_count / ROAD_CELLS_PER_SEND` sends,
/// one every [`ROAD_SEND_SECONDS`]. 50 cells is about 5 kB a send.
pub const ROAD_CELLS_PER_SEND: usize = 50;
/// Seconds between round-robin sends. The road moves over minutes, so the
/// lag around the lap is invisible and the bandwidth is bounded.
pub const ROAD_SEND_SECONDS: f32 = 2.0;
/// Cells in one message of the whole-lap burst a joining client gets.
pub const ROAD_CELLS_PER_JOIN: usize = 512;

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

/// The water a snow sky keeps on the road, in the weather's units: the slush
/// under the flakes (`Weather::Snow.water()`), which the treaded tyre's
/// territory is read off.
pub const SNOW_WATER: f32 = 0.3;
/// Share of the gap to full cover the snowfall closes per second: a bare
/// road is white in ten minutes.
pub const SNOWFALL_RATE: f32 = 0.004;
/// Share of the cover above [`SNOW_FLOOR`] one wheel's pass packs down.
pub const SNOW_CLEAR_PER_PASS: f32 = 0.08;
/// The cover a driven line is packed to and no further: a film of snow and
/// slush that grips (1 - 0.72 * 0.65^0.5) / 0.28 = 1.5 times a covered road.
pub const SNOW_FLOOR: f32 = 0.65;
/// What full cover costs the road's grip: bare 1.0, covered 1 - this. A
/// racing tyre on packed snow has a quarter to a third of its dry grip.
pub const SNOW_COVER_GRIP: f32 = 0.72;
/// The share of a wheel's pass the pit lane's one cover figure a cell takes
/// ([`RoadState::lane_snow`]): the cars' wheel tracks cover about a quarter
/// of the lane's width, so it packs at a quarter of a racing bin's rate.
pub const LANE_PACK_SHARE: f32 = 0.25;
/// Share of the cover a degree of track temperature melts per second.
pub const SNOW_MELT_PER_C: f32 = 0.0004;
/// The track freezes the water on it below this, °C.
pub const FREEZE_C: f32 = -1.0;
/// Share of the gap to solid ice a freezing, watery bin closes per second.
pub const ICE_FREEZE_RATE: f32 = 0.002;
/// Ice melts this share per degree of track temperature per second.
pub const ICE_THAW_PER_C: f32 = 0.0006;
/// What solid ice costs the grip: a tenth of the dry road's under a
/// covered bake.
pub const ICE_GRIP_LOSS: f32 = 0.6;
/// A bin under more cover than this has no bare water to freeze.
const ICE_COVER_MAX: f32 = 0.9;
/// Falling snow covers the water before it freezes: no ice forms in a
/// snowfall of this much or more.
const ICE_SNOWFALL_MAX: f32 = 0.1;
/// A bin with less water than this does not freeze.
const ICE_WATER_MIN: f32 = 0.05;

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

/// The grip snow and ice leave a road, against the road baked at
/// `baked_cover`: the cover's own (bare 1.0, covered `1 - SNOW_COVER_GRIP`,
/// by the root of the cover: a thin film is most of the way to a covered
/// road) against the baked one, less the ice. 1.0 for a road without
/// either, whatever the sky.
pub fn snow_grip(cover: f32, ice: f32, baked_cover: f32) -> f32 {
    let cover_grip = 1.0 - SNOW_COVER_GRIP * cover.clamp(0.0, 1.0).sqrt();
    let baked = 1.0 - SNOW_COVER_GRIP * baked_cover.clamp(0.0, 1.0).sqrt();
    cover_grip / baked * (1.0 - ICE_GRIP_LOSS * ice.clamp(0.0, 1.0))
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
    /// Marbles lying there, 0..1: what a tyre picks up.
    pub marbles: f32,
    /// The snow cover (0..1) and the ice (0..1) on the racing surface
    /// there: what a snow tyre on the road or a curb bites into
    /// (`tyre_thermal::Compound::snow_factor`).
    pub snow: f32,
    pub ice: f32,
    /// The cover on the asphalt nobody races on (`wet_grip`'s: the verge's
    /// beside the run-off, the lane's own in the pit lane), and on the
    /// grass (`off_grip`'s).
    pub paved_snow: f32,
    pub verge_snow: f32,
}

impl RoadSample {
    /// A road exactly as baked.
    pub const BAKED: RoadSample = RoadSample {
        water: 0.0,
        grip: 1.0,
        wet_grip: 1.0,
        off_grip: 1.0,
        curb_grip: 1.0,
        marbles: 0.0,
        snow: 0.0,
        ice: 0.0,
        paved_snow: 0.0,
        verge_snow: 0.0,
    };

    /// A road exactly as baked at a snow `cover` (`TrackSurface::baked_snow`)
    /// everywhere: what a track without a road state grips as.
    pub fn baked(water: f32, cover: f32) -> RoadSample {
        RoadSample {
            water,
            snow: cover,
            paved_snow: cover,
            verge_snow: cover,
            ..Self::BAKED
        }
    }
}

/// One cell of the lap.
#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
pub struct Cell {
    /// Water, in the weather's units (1 is heavy rain on a flat road).
    pub depth: f32,
    /// How low the road lies here against its surroundings, 0..1: a dip
    /// along the lap, or a banked road's low edge ([`edge_low`]).
    pub low: f32,
    /// The snow on the verge, 0..1: no wheel clears it.
    #[serde(default)]
    pub verge_snow: f32,
    /// The raceline's lateral offset at the cell, m, positive right.
    pub line_lateral_m: f32,
    /// Where the cell's puddle lies across the road, m right of the
    /// centerline, and its half-width, m (see [`RoadState::bin_depth`]).
    #[serde(default)]
    pub puddle_lateral_m: f32,
    #[serde(default)]
    pub puddle_half_m: f32,
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
    /// The snow falling now, 0 or 1 (`Weather::snowfall`, eased by the
    /// session like `rain`).
    #[serde(default)]
    pub snowfall: f32,
    /// The asphalt now, °C: it melts the snow and thaws the ice, and below
    /// [`FREEZE_C`] it freezes the water on it. Set by the session.
    #[serde(default)]
    pub track_c: f32,
    /// The water the session's road grip was baked at
    /// (`TrackSurface::baked_water`): every grip figure is against it.
    pub baked_water: f32,
    /// The cover the road's grip was baked at (`TrackSurface::baked_snow`).
    #[serde(default)]
    pub baked_snow: f32,
    pub lap_m: f32,
    pub cells: Vec<Cell>,
    /// Per bin, [`BINS`] to a cell, cell after cell.
    pub dry: Vec<f32>,
    pub rubber: Vec<f32>,
    pub marbles: Vec<f32>,
    /// Snow cover and ice per bin, 0..1.
    #[serde(default)]
    pub snow: Vec<f32>,
    /// The pit lane's snow cover, 0..1, one figure a [`CELL_M`] cell of the
    /// lane's own station (`PitLane::length_m`), empty for a track without a
    /// lane: the cars in it pack it ([`RoadState::note_lane_pass`]), the
    /// snowfall covers it, a warm track melts it.
    #[serde(default)]
    pub lane_snow: Vec<f32>,
    #[serde(default)]
    pub ice: Vec<f32>,
    /// Whether any snow or ice lies on the road or is falling (as of the
    /// last step): a road without leaves the snow grip out of the sample.
    #[serde(skip)]
    snowy: bool,
    /// Wheel passes per bin since the last step, in passes' worth (a
    /// wheel on its share of the car's weight is one), and the rubber they
    /// laid, in passes' worth of sliding.
    #[serde(skip)]
    wheel_passes: Vec<f32>,
    #[serde(skip)]
    wheel_rubber: Vec<f32>,
    /// Wheel passes per lane cell since the last step, in passes' worth.
    #[serde(skip)]
    lane_passes: Vec<f32>,
}

impl RoadState {
    /// The road for `track` under its baked weather
    /// (`track_surface.water`), already standing: the rain has been
    /// falling a while, so the flat road holds the weather's figure and the
    /// dips their puddles, no line is dry yet, and the raceline carries
    /// `rubber` (0 green to 1 fully rubbered; [`RUBBER_REFERENCE`] is what
    /// the cars were calibrated on).
    pub fn new(track: &TrackConfig, rubber: f32) -> Self {
        let baked_snow = track.track_surface.baked_snow.clamp(0.0, 1.0);
        // The slush a snow sky keeps on the road is its own water, not rain.
        let rain = (track.track_surface.water - SNOW_WATER * baked_snow).max(0.0);
        let lap_m = crate::laps::track_length_m(track).max(CELL_M);
        let n = (lap_m / CELL_M).ceil().max(1.0) as usize;
        let mut cells = vec![Cell::default(); n];
        let mut zs = Vec::with_capacity(n);
        let mut banks = Vec::with_capacity(n);
        let mut hint = None;
        for (i, cell) in cells.iter_mut().enumerate() {
            let station = (i as f32 + 0.5) * CELL_M;
            if track.centerline.is_empty() {
                zs.push(0.0);
                banks.push(0.0);
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
            // The water runs to the low edge of a banked road (positive
            // banking lowers the right); on a flat one it stands where the
            // asphalt happens to sag, a seeded spot.
            let bank = banking_at(track, station);
            banks.push(bank);
            let spread = |salt| crate::wind::hash01(i as u64, salt);
            cell.puddle_half_m =
                PUDDLE_HALF_MIN_M + PUDDLE_HALF_SPREAD_M * spread(PUDDLE_WIDTH_SALT);
            cell.puddle_lateral_m = if bank > PUDDLE_BANK_RAD {
                right - 0.5 * cell.puddle_half_m
            } else if bank < -PUDDLE_BANK_RAD {
                -left + 0.5 * cell.puddle_half_m
            } else {
                -left + (left + right) * spread(PUDDLE_SPOT_SALT)
            };
            let (lateral, found) = raceline_lateral_at(track, x, y, heading, hint);
            cell.line_lateral_m = lateral;
            hint = found;
        }
        // How low each cell lies: the road's elevation against the mean of
        // its surroundings along the lap, or, where the road is banked or
        // cambered, its low edge, which the whole width drains to (a
        // stream along the inside of a cambered corner); a dip widens its
        // puddle, the edge's stays a strip.
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
            let dip = ((mean - zs[i]) / LOW_DEPTH_M).clamp(0.0, 1.0);
            cells[i].low = dip.max(edge_low(banks[i]));
            cells[i].puddle_half_m += PUDDLE_HALF_LOW_M * dip;
            cells[i].depth = Self::standing_depth(rain + SNOW_WATER * baked_snow, cells[i].low);
            cells[i].verge_snow = baked_snow;
        }
        let mut snow_bins = vec![0.0; n * BINS];
        if baked_snow > 0.0 {
            for (i, cell) in cells.iter().enumerate() {
                for b in cell.first as usize..=cell.last as usize {
                    snow_bins[i * BINS + b] = baked_snow;
                }
            }
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
        let lane_cells = track.pit_lane.as_ref().map_or(0, |lane| {
            (lane.length_m.max(0.0) / CELL_M).ceil().max(1.0) as usize
        });
        Self {
            rain,
            snowfall: baked_snow,
            track_c: track.track_surface.track_temperature_c,
            baked_water: track.track_surface.baked_water,
            baked_snow,
            lap_m,
            cells,
            dry: vec![0.0; n * BINS],
            rubber: rubber_bins,
            marbles: vec![0.0; n * BINS],
            snow: snow_bins,
            lane_snow: vec![baked_snow; lane_cells],
            ice: vec![0.0; n * BINS],
            snowy: baked_snow > 0.0,
            wheel_passes: vec![0.0; n * BINS],
            wheel_rubber: vec![0.0; n * BINS],
            lane_passes: vec![0.0; lane_cells],
        }
    }

    /// The pit lane's cell at `lane_station_m` along the lane, if the track
    /// has a lane.
    fn lane_cell(&self, lane_station_m: f32) -> Option<usize> {
        let n = self.lane_snow.len();
        (n > 0).then(|| ((lane_station_m.max(0.0) / CELL_M) as usize).min(n - 1))
    }

    /// The pit lane's snow cover at `lane_station_m` along the lane, 0..1;
    /// none for a track without a lane.
    pub fn lane_snow_at(&self, lane_station_m: f32) -> Option<f32> {
        self.lane_cell(lane_station_m).map(|i| self.lane_snow[i])
    }

    /// A car in the pit lane crossed a cell: its wheels, `weight` passes'
    /// worth (the sum of [`WheelPass::weight`]), pack the lane's cover at
    /// `lane_station_m` along the lane.
    pub fn note_lane_pass(&mut self, lane_station_m: f32, weight: f32) {
        if let Some(i) = self.lane_cell(lane_station_m) {
            if let Some(passes) = self.lane_passes.get_mut(i) {
                *passes += weight.max(0.0);
            }
        }
    }

    /// The depth a cell `low` settles at under `rain`.
    fn standing_depth(rain: f32, low: f32) -> f32 {
        rain * (1.0 + PUDDLE_GAIN * low)
    }

    /// The share of a cell's depth that stands at `lateral_m` (m right of
    /// the centerline): all of it in the puddle, the flat road's share
    /// (`1 / (1 + PUDDLE_GAIN x low)`) away from it, eased between. A cell
    /// that is not low is the same all across.
    fn depth_share(cell: &Cell, lateral_m: f32) -> f32 {
        if cell.low <= 0.0 {
            return 1.0;
        }
        let flat = 1.0 / (1.0 + PUDDLE_GAIN * cell.low);
        let off = (lateral_m - cell.puddle_lateral_m) / cell.puddle_half_m.max(0.1);
        let inside = (1.0 - off * off).max(0.0);
        flat + (1.0 - flat) * inside * inside
    }

    /// The water standing on bin `b` of cell `i`, before the wheels dry it:
    /// the cell's depth where its puddle lies, the flat road's elsewhere.
    pub fn bin_depth(&self, i: usize, b: usize) -> f32 {
        let cell = &self.cells[i];
        cell.depth * Self::depth_share(cell, bin_centre(b))
    }

    /// How many cells the lap is cut into.
    pub fn cell_count(&self) -> usize {
        self.cells.len()
    }

    /// Pack where `count` cells from `first_cell` (wrapping) lie, for the
    /// wire ([`GEOMETRY_BYTES`] each): the centerline at the cell's middle
    /// and the unit vector to its left, four little-endian `f32`s in the
    /// server frame. A bin's lateral is measured from exactly this point
    /// along exactly this vector ([`RoadState::lateral_of`]), so the client
    /// that draws the bins from world position lines up with the sim.
    pub fn write_geometry(&self, first_cell: usize, count: usize, out: &mut Vec<u8>) {
        let n = self.cells.len();
        if n == 0 {
            return;
        }
        for k in 0..count {
            let c = &self.cells[(first_cell + k) % n];
            for v in [c.x, c.y, c.left_x, c.left_y] {
                out.extend_from_slice(&v.to_le_bytes());
            }
        }
    }

    /// Pack `count` cells from `first_cell` (wrapping the lap) for the wire
    /// ([`ROW_BYTES`] each): the cell's water depth as a byte in
    /// [`DEPTH_BYTE`] units (100 is heavy rain on the flat, over 100 a
    /// puddle), then its [`BINS`] bins, each rubber, marbles and dry as a
    /// byte over 0..1. The client reads it straight into a texture row; a
    /// byte is the client's resolution, the sim keeps its `f32`s. Bins the
    /// road does not cover stay 0 (the tyre never samples them).
    pub fn write_rows(&self, first_cell: usize, count: usize, out: &mut Vec<u8>) {
        let n = self.cells.len();
        if n == 0 {
            return;
        }
        let q = |v: f32| (v * 255.0).round().clamp(0.0, 255.0) as u8;
        for k in 0..count {
            let i = (first_cell + k) % n;
            let cell = &self.cells[i];
            out.push((cell.depth * DEPTH_BYTE).round().clamp(0.0, 255.0) as u8);
            let base = i * BINS;
            for b in 0..BINS {
                out.push(q(self.rubber[base + b]));
                out.push(q(self.marbles[base + b]));
                out.push(q(self.dry[base + b]));
            }
        }
    }

    /// Pack the snow of the same `count` cells from `first_cell`
    /// ([`SNOW_ROW_BYTES`] each): the verge's cover, then per bin the cover
    /// and the ice, bytes over 0..1. A separate field of the `RoadState`
    /// message, so a client from before snow reads the rows it knows.
    pub fn write_snow_rows(&self, first_cell: usize, count: usize, out: &mut Vec<u8>) {
        let n = self.cells.len();
        if n == 0 {
            return;
        }
        let q = |v: f32| (v * 255.0).round().clamp(0.0, 255.0) as u8;
        for k in 0..count {
            let i = (first_cell + k) % n;
            out.push(q(self.cells[i].verge_snow));
            let base = i * BINS;
            for b in 0..BINS {
                out.push(q(self.snow[base + b]));
                out.push(q(self.ice[base + b]));
            }
        }
    }

    /// Pack the pit lane's snow cover, one byte over 0..1 per [`CELL_M`]
    /// of the lane's own station (`lane_snow`); nothing for a track without
    /// a lane. The whole lane goes in every `RoadState` slice: a few dozen
    /// bytes, so the lane is never out of step with the road.
    pub fn write_lane_snow(&self, out: &mut Vec<u8>) {
        out.extend(
            self.lane_snow
                .iter()
                .map(|v| (v * 255.0).round().clamp(0.0, 255.0) as u8),
        );
    }

    /// Pack the water standing on each bin of the same `count` cells from
    /// `first_cell` ([`BINS`] bytes each, in [`DEPTH_BYTE`] units, before
    /// the wheels dry it: [`RoadState::bin_depth`]), so a puddle is drawn
    /// where it lies across the road. A separate field of the `RoadState`
    /// message, left off for a road with no puddle standing
    /// ([`RoadState::has_puddles`]); a client without it spreads the
    /// cell's depth (the row's first byte) across the road.
    pub fn write_water_rows(&self, first_cell: usize, count: usize, out: &mut Vec<u8>) {
        let n = self.cells.len();
        if n == 0 {
            return;
        }
        for k in 0..count {
            let i = (first_cell + k) % n;
            let cell = &self.cells[i];
            for b in 0..BINS {
                let on_road = b >= cell.first as usize && b <= cell.last as usize;
                let depth = if on_road { self.bin_depth(i, b) } else { 0.0 };
                out.push((depth * DEPTH_BYTE).round().clamp(0.0, 255.0) as u8);
            }
        }
    }

    /// Whether water stands deeper in a dip than on the flat anywhere on
    /// the lap: a road without leaves the per-bin water off the wire.
    pub fn has_puddles(&self) -> bool {
        self.cells.iter().any(|c| c.low > 0.0 && c.depth > 0.0)
    }

    /// Whether any snow or ice lies anywhere on the road, or is falling:
    /// a session without leaves the snow field off the wire.
    pub fn has_snow(&self) -> bool {
        self.baked_snow > 0.0 || self.snowy
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
        self.sample_with(station_m, lateral_m, None)
    }

    /// What a tyre in the pit lane finds: [`Self::sample`] at its place on
    /// the lap, its `wet_grip` (the lane's asphalt) under the lane's own
    /// cover at `lane_station_m` along the lane rather than the verge's
    /// beside the lap's cell.
    pub fn sample_in_lane(
        &self,
        station_m: f32,
        lateral_m: f32,
        lane_station_m: f32,
    ) -> RoadSample {
        self.sample_with(station_m, lateral_m, self.lane_snow_at(lane_station_m))
    }

    fn sample_with(&self, station_m: f32, lateral_m: f32, lane_cover: Option<f32>) -> RoadSample {
        if self.cells.is_empty() {
            return RoadSample::BAKED;
        }
        let i = self.cell_index(station_m);
        let cell = &self.cells[i];
        let base = i * BINS;
        let across = Self::across(cell, lateral_m);
        let dry = Self::bin_value(&self.dry, base, across);
        let lateral = (across.0 as f32 + across.2 + 0.5) * BIN_M - HALF_SPAN_M;
        let water = (cell.depth * Self::depth_share(cell, lateral) * (1.0 - dry)).max(0.0);
        let rubber = Self::bin_value(&self.rubber, base, across);
        let marbles = Self::bin_value(&self.marbles, base, across);
        let baked = self.baked_water;
        let mut wet = road_grip_for(water) / road_grip_for(baked).max(1e-3);
        let rubbered = rubber_grip(rubber, water) * (1.0 - MARBLE_GRIP * marbles.clamp(0.0, 1.0));
        let mut grip = wet * rubbered;
        // The grass holds the cell's water: nobody dries it.
        let mut off_grip = road_grip_for(cell.depth) * off_track_grip_for(cell.depth)
            / (road_grip_for(baked) * off_track_grip_for(baked)).max(1e-3);
        let mut snow = [0.0; 4];
        // Snow and ice only move a road that has some or was baked with it.
        if self.baked_snow > 0.0 || self.snowy {
            let cover = Self::bin_value(&self.snow, base, across);
            let ice = Self::bin_value(&self.ice, base, across);
            let verge = cell.verge_snow;
            let paved = lane_cover.unwrap_or(verge);
            // The slush is the cover's own (its grip is in `snow_grip`):
            // the water counts as water only as far as the road is bare,
            // so a road melted clear grips as wet or dry asphalt does, and
            // a covered one as the cover, whatever water is under it.
            let under = |w: f32, c: f32| w * (1.0 - c.clamp(0.0, 1.0).sqrt());
            let road = |w: f32, c: f32| road_grip_for(under(w, c));
            let grass = |w: f32, c: f32| road(w, c) * off_track_grip_for(under(w, c));
            let reference = road(baked, self.baked_snow).max(1e-3);
            grip =
                road(water, cover) / reference * rubbered * snow_grip(cover, ice, self.baked_snow);
            // The run-off, which no racing wheel clears, carries the
            // verge's cover: buried under a snowfall, bare once the track
            // melts it. The pit lane carries its own, which its cars pack.
            wet = road(water, paved) / reference * snow_grip(paved, 0.0, self.baked_snow);
            off_grip = grass(cell.depth, verge) / grass(baked, self.baked_snow).max(1e-3)
                * snow_grip(verge, 0.0, self.baked_snow);
            snow = [cover, ice, paved, verge].map(|v| v.clamp(0.0, 1.0));
        }
        let [snow, ice, paved_snow, verge_snow] = snow;
        RoadSample {
            water,
            grip,
            wet_grip: wet,
            off_grip,
            curb_grip: curb_grip_for(water) / curb_grip_for(baked).max(1e-3),
            marbles: marbles.clamp(0.0, 1.0),
            snow,
            ice,
            paved_snow,
            verge_snow,
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
                    * Self::depth_share(c, c.line_lateral_m)
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

    /// The snow cover on the line a driver choosing tyres reads
    /// ([`Self::mean_line_snow`]), 0 on a road that has none, and the bake's
    /// cover on a track without a road state.
    pub fn line_snow_of(track: &TrackConfig) -> f32 {
        match track.road_state.as_ref() {
            Some(road) if road.has_snow() => road.mean_line_snow(),
            Some(_) => 0.0,
            None => track.track_surface.baked_snow.clamp(0.0, 1.0),
        }
    }

    /// The snow cover on the raceline across the lap, the cells' mean at
    /// the line, 0..1: what a driver deciding on tyres and a pace looks at.
    pub fn mean_line_snow(&self) -> f32 {
        if self.cells.is_empty() {
            return 0.0;
        }
        let sum: f32 = self
            .cells
            .iter()
            .enumerate()
            .map(|(i, c)| Self::bin_value(&self.snow, i * BINS, Self::across(c, c.line_lateral_m)))
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

    /// A car crossed into the cell at `station_m`: each of its `wheels`
    /// lays its pass in the bin its patch runs in ([`WheelPass`]: how much
    /// of a pass its load makes it, how much rubber its sliding lays), and
    /// the car sheds `shed` passes' worth of marbles over the cell (the
    /// tread its tyres wore since the last cell: [`shed_for_wear`]).
    pub fn note_pass(&mut self, station_m: f32, wheels: &[WheelPass], shed: f32) {
        if self.cells.is_empty() {
            return;
        }
        let i = self.cell_index(station_m);
        let cell = &mut self.cells[i];
        cell.passes = cell.passes.saturating_add(1);
        cell.shed += shed.max(0.0);
        for wheel in wheels {
            let j = self.cell_index(station_m + wheel.ahead_m);
            if let Some(b) = bin_of(wheel.lateral_m) {
                self.wheel_passes[j * BINS + b] += wheel.weight.max(0.0);
                self.wheel_rubber[j * BINS + b] += wheel.rubber.max(0.0);
            }
        }
    }

    /// Advance the road by `dt` seconds (the ticks since the last step):
    /// the rain, the drainage (`evaporation` scales it: 1 under a mild
    /// sky, more under a high sun on warm asphalt), the passes' drying,
    /// rubber and marbles, and the rain's rewetting and washing.
    pub fn step(&mut self, dt: f32, evaporation: f32) {
        let rain = self.rain.max(0.0);
        let snowfall = self.snowfall.max(0.0);
        // Water falls as rain, or as the slush under the snow.
        let falling = rain + SNOW_WATER * snowfall;
        let ceiling = line_dry_ceiling(falling);
        let drain = DRAIN_RATE * evaporation.max(0.0);
        let freezing = self.track_c < FREEZE_C;
        let warm = self.track_c.max(0.0);
        let melt = SNOW_MELT_PER_C * warm * dt;
        let thaw = ICE_THAW_PER_C * warm * dt;
        let settle = (SNOWFALL_RATE * snowfall * dt).min(1.0);
        let mut any_snow = false;
        for (i, cell) in self.cells.iter_mut().enumerate() {
            let target = Self::standing_depth(falling, cell.low);
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
            if snowfall > 0.0 || cell.verge_snow > 0.0 {
                // The verge: no wheel clears it.
                cell.verge_snow += (1.0 - cell.verge_snow) * settle;
                if melt > 0.0 {
                    cell.verge_snow = (cell.verge_snow - melt).max(0.0);
                }
                any_snow |= cell.verge_snow > 0.0;
            }
            let wet = (cell.depth / 0.5).clamp(0.0, 1.0);
            cell.passes = 0;
            let car_passes = std::mem::take(&mut cell.shed);
            let base = i * BINS;
            let (first, last) = (cell.first as usize, cell.last as usize);
            for b in first..=last {
                let k = base + b;
                // A wheel in this bin counts in full, one in the next bin
                // either way in part.
                let spread = |v: &[f32]| {
                    let near = if b > first { v[k - 1] } else { 0.0 }
                        + if b < last { v[k + 1] } else { 0.0 };
                    v[k] + NEIGHBOUR_SHARE * near
                };
                let passes = spread(&self.wheel_passes);
                let rubber_passes = spread(&self.wheel_rubber);
                // The passes wipe the water; the rain wets it again.
                let dry = &mut self.dry[k];
                *dry += (1.0 - *dry) * (DRY_PER_PASS * passes).min(1.0);
                *dry -= *dry * REWET_RATE * falling * dt;
                *dry = dry.clamp(0.0, ceiling);
                // Snow settles, the passes pack it down to a driven line,
                // a warm track melts it; the water on a cleared bin freezes
                // on a cold one.
                let cover = &mut self.snow[k];
                if snowfall > 0.0 {
                    *cover += (1.0 - *cover) * settle;
                }
                if *cover > SNOW_FLOOR && passes > 0.0 {
                    *cover -= (*cover - SNOW_FLOOR) * (SNOW_CLEAR_PER_PASS * passes).min(1.0);
                }
                if *cover > 0.0 && melt > 0.0 {
                    *cover = (*cover - melt).max(0.0);
                }
                let cover = *cover;
                let ice = &mut self.ice[k];
                if freezing
                    && cover < ICE_COVER_MAX
                    && snowfall < ICE_SNOWFALL_MAX
                    && cell.depth * Self::depth_share(cell, bin_centre(b)) * (1.0 - *dry)
                        > ICE_WATER_MIN
                {
                    *ice += (1.0 - *ice) * (ICE_FREEZE_RATE * dt).min(1.0);
                } else if *ice > 0.0 && thaw > 0.0 {
                    *ice = (*ice - thaw).max(0.0);
                }
                any_snow |= cover > 0.0 || *ice > 0.0;
                // Snow on a bin keeps the rubber and the marbles off it as
                // water does.
                let gate = 1.0 - wet.max(cover);
                // Dry passes lay rubber; rain washes it.
                let rubber = &mut self.rubber[k];
                *rubber += (1.0 - *rubber) * (RUBBER_PER_PASS * rubber_passes * gate).min(1.0);
                *rubber -= *rubber * (RUBBER_WASH * rain * dt).min(1.0);
                *rubber = rubber.clamp(0.0, 1.0);
                // Every car sheds over the cell; every wheel sweeps its bin.
                let marbles = &mut self.marbles[k];
                *marbles += MARBLES_PER_PASS * car_passes * gate;
                *marbles *= (1.0 - MARBLE_SWEEP).powf(passes);
                *marbles -= *marbles * (MARBLE_WASH * rain * dt).min(1.0);
                *marbles = marbles.clamp(0.0, 1.0);
            }
            for b in first..=last {
                self.wheel_passes[base + b] = 0.0;
                self.wheel_rubber[base + b] = 0.0;
            }
        }
        // The pit lane: the snowfall covers it, its cars pack it toward the
        // floor as the racing bins' wheels do (a quarter of the rate: one
        // figure across the lane), a warm track melts it.
        for (cover, passes) in self.lane_snow.iter_mut().zip(self.lane_passes.iter_mut()) {
            let packed = std::mem::take(passes);
            if snowfall <= 0.0 && *cover <= 0.0 {
                continue;
            }
            *cover += (1.0 - *cover) * settle;
            if *cover > SNOW_FLOOR && packed > 0.0 {
                *cover -= (*cover - SNOW_FLOOR)
                    * (SNOW_CLEAR_PER_PASS * LANE_PACK_SHARE * packed).min(1.0);
            }
            if melt > 0.0 {
                *cover = (*cover - melt).max(0.0);
            }
            any_snow |= *cover > 0.0;
        }
        self.snowy = snowfall > 0.0 || any_snow;
    }
}

/// One wheel's pass through a cell ([`RoadState::note_pass`]).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct WheelPass {
    /// Where its contact patch runs: m along the lap ahead of the car's
    /// station, m right of the centerline.
    pub ahead_m: f32,
    pub lateral_m: f32,
    /// How much of a pass it makes, by the load on it: a wheel on its
    /// share of the car's weight is one, a lifted one nothing
    /// ([`pass_weights`]). What wipes the water, packs the snow and sweeps
    /// the marbles.
    pub weight: f32,
    /// The rubber it lays, in passes' worth: its sliding against
    /// [`RUBBER_SLIDE_J`] ([`rubber_passes`]).
    pub rubber: f32,
}

impl WheelPass {
    /// A wheel at `lateral_m` on its share of the weight, laying a pass's
    /// rubber.
    pub fn plain(lateral_m: f32) -> Self {
        Self {
            ahead_m: 0.0,
            lateral_m,
            weight: 1.0,
            rubber: 1.0,
        }
    }
}

/// What one wheel did since its car last crossed into a cell, as the
/// physics adds it up tick by tick ([`RoadWork::add`]).
#[derive(Debug, Clone, Copy, Default, PartialEq, Serialize, Deserialize)]
pub struct WheelRoadWork {
    /// Energy its patch dissipated sliding, J.
    pub slide_j: f32,
    /// Its load over the time, N s.
    pub load_ns: f32,
    /// Tread it wore, percent.
    pub wear_pct: f32,
}

/// A car's work on the road since it last crossed into a cell, and where
/// its patches run (`CarState::road_work`): `GameSession::update_road`
/// lays it as the car's passes when it crosses into the next.
#[derive(Debug, Clone, Copy, Default, PartialEq, Serialize, Deserialize)]
pub struct RoadWork {
    /// FL FR RL RR.
    pub wheels: [WheelRoadWork; 4],
    pub seconds: f32,
    /// Each patch as of the last tick: m along the lap ahead of the car's
    /// station, m right of the centerline (where the physics samples the
    /// road for that tyre).
    pub patch_ahead_m: [f32; 4],
    pub patch_lateral_m: [f32; 4],
}

impl RoadWork {
    /// One tick of `dt`: each wheel's sliding power (W), load (N) and the
    /// tread it wore (percent).
    pub fn add(&mut self, dt: f32, slide_w: [f32; 4], load_n: [f32; 4], wear_pct: [f32; 4]) {
        for (i, w) in self.wheels.iter_mut().enumerate() {
            w.slide_j += slide_w[i].max(0.0) * dt;
            w.load_ns += load_n[i].max(0.0) * dt;
            w.wear_pct += wear_pct[i].max(0.0);
        }
        self.seconds += dt;
    }

    /// The four wheels' passes for a car weighing `weight_n`.
    pub fn passes(&self, weight_n: f32) -> [WheelPass; 4] {
        let weights = pass_weights(self, weight_n);
        std::array::from_fn(|i| WheelPass {
            ahead_m: self.patch_ahead_m[i],
            lateral_m: self.patch_lateral_m[i],
            weight: weights[i],
            rubber: rubber_passes(self.wheels[i].slide_j),
        })
    }

    /// The marbles the car shed since the last cell ([`shed_for_wear`]).
    pub fn shed(&self) -> f32 {
        shed_for_wear(self.wheels.iter().map(|w| w.wear_pct).sum())
    }

    /// Start the next cell's count, keeping where the patches run.
    pub fn restart(&mut self) {
        self.wheels = Default::default();
        self.seconds = 0.0;
    }
}

/// Each wheel's share of a pass by its mean load: four times its share of
/// the car's load, the load held to the car's weight `weight_n` at least
/// (downforce presses a car's passes no harder than its weight does, so a
/// field counts as many passes as when every wheel counted alike; a wheel
/// in the air counts nothing, the loaded outside wheels of a corner more
/// than the inside).
pub fn pass_weights(work: &RoadWork, weight_n: f32) -> [f32; 4] {
    if work.seconds <= 0.0 {
        return [0.0; 4];
    }
    let loads: [f32; 4] = std::array::from_fn(|i| work.wheels[i].load_ns / work.seconds);
    let total = loads.iter().sum::<f32>().max(weight_n).max(1.0);
    std::array::from_fn(|i| (4.0 * loads[i] / total).clamp(0.0, MAX_PASS_WEIGHT))
}

/// The most of a pass one wheel makes by its load.
const MAX_PASS_WEIGHT: f32 = 2.5;

/// The sliding energy one wheel spends through a cell that lays a pass's
/// worth of rubber ([`RUBBER_PER_PASS`]), J: an AI GT3's mean over a lap
/// of Monza (some 690 J), so a field lays about as much as when every pass
/// counted alike, the most
/// where the tyres work hardest (the braking zones, the corners) and next
/// to none on a straight.
pub const RUBBER_SLIDE_J: f32 = 700.0;
/// The most rubber one wheel's pass lays, in passes' worth.
const MAX_RUBBER_PASSES: f32 = 6.0;

/// The rubber a wheel lays for `slide_j` of sliding, in passes' worth.
pub fn rubber_passes(slide_j: f32) -> f32 {
    (slide_j.max(0.0) / RUBBER_SLIDE_J).min(MAX_RUBBER_PASSES)
}

/// The tread a car's four tyres wear through a cell that sheds a pass's
/// worth of marbles ([`MARBLES_PER_PASS`]), percent: an AI GT3 wears some
/// 0.02% a cell at Monza, a third of a pass's worth, what it shed by its
/// lateral g before the marbles came from the wear.
pub const MARBLE_WEAR_PCT: f32 = 0.06;
/// The most marbles a car sheds in one cell, in passes' worth.
const MAX_SHED: f32 = 4.0;

/// How much a car whose tyres wore `wear_pct` (percent, the four summed)
/// since its last cell sheds as marbles, in passes' worth: the tread worn
/// away is what rolls off as marbles, so a car sheds them where its tyres
/// work (the corners, the braking zones) and by how fast its compound
/// wears, and next to none rolling straight.
pub fn shed_for_wear(wear_pct: f32) -> f32 {
    (wear_pct.max(0.0) / MARBLE_WEAR_PCT).min(MAX_SHED)
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

/// How low the low edge of a road banked `bank_rad` lies, as a cell's
/// `low`: none on a flat road, [`EDGE_LOW_MAX`] from some 2 degrees.
pub fn edge_low(bank_rad: f32) -> f32 {
    ((bank_rad.abs() - PUDDLE_BANK_RAD) / EDGE_BANK_FULL_RAD).clamp(0.0, EDGE_LOW_MAX)
}

/// The road's banking at `station_m`, rad (positive lowers the right).
fn banking_at(track: &TrackConfig, station_m: f32) -> f32 {
    let cl = &track.centerline;
    let i = cl
        .partition_point(|p| p.distance_from_start_m < station_m)
        .min(cl.len() - 1);
    cl[i].banking_rad
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
    use crate::data::{RacelinePoint, TrackPoint, Weather};

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
                    &[
                        WheelPass::plain(lateral - 0.8),
                        WheelPass::plain(lateral + 0.8),
                    ],
                    1.0,
                );
            }
            road.step(dt, 1.0);
        }
    }

    /// The dipped track baked under a snow sky: slush on the road, a
    /// covered road's grip, the asphalt at `track_c`.
    fn snowy_track(track_c: f32) -> TrackConfig {
        let mut track = dipped_track(SNOW_WATER);
        track.track_surface.baked_snow = 1.0;
        track.track_surface.track_temperature_c = track_c;
        track
    }

    #[test]
    fn snow_costs_grip_by_its_cover_and_ice_by_its_own() {
        assert_eq!(snow_grip(0.0, 0.0, 0.0), 1.0, "bare against bare");
        assert_eq!(snow_grip(1.0, 0.0, 1.0), 1.0, "covered against covered");
        assert!(
            (snow_grip(1.0, 0.0, 0.0) - 0.28).abs() < 1e-6,
            "snow on a dry bake"
        );
        assert!(
            (snow_grip(SNOW_FLOOR, 0.0, 1.0) - 1.498).abs() < 1e-3,
            "a driven line"
        );
        assert!((snow_grip(1.0, 1.0, 1.0) - 0.4).abs() < 1e-6, "ice");
        assert!(snow_grip(0.0, 0.0, 1.0) > 3.5, "melted off a covered bake");
    }

    #[test]
    fn a_snow_session_starts_covered_and_grips_as_baked() {
        let road = RoadState::new(&snowy_track(-3.0), RUBBER_REFERENCE);
        assert_eq!(road.snowfall, 1.0);
        assert_eq!(road.rain, 0.0, "the slush is not rain");
        assert!(road.has_snow());
        for lateral in [-5.0, 2.0, 6.0] {
            let s = road.sample(500.0, lateral);
            assert!((s.grip - 1.0).abs() < 1e-4, "at {lateral}: {}", s.grip);
            assert!((s.water - SNOW_WATER).abs() < 0.02, "slush: {}", s.water);
        }
        assert!((road.mean_line_snow() - 1.0).abs() < 1e-6);
        // A dry road has none of it.
        let dry = RoadState::new(&dipped_track(0.0), RUBBER_REFERENCE);
        assert!(!dry.has_snow() && dry.mean_line_snow() == 0.0);
    }

    /// What a snow tyre bites into: the sample carries the cover and ice
    /// on the racing surface, the run-off's, the lane's and the verge's;
    /// none of it off a snow session.
    #[test]
    fn a_sample_carries_the_cover_under_each_surface() {
        let mut track = snowy_track(-3.0);
        let mut road = RoadState::new(&track, RUBBER_REFERENCE);
        let s = road.sample(500.0, 2.0);
        assert_eq!(
            [s.snow, s.ice, s.paved_snow, s.verge_snow],
            [1.0, 0.0, 1.0, 1.0]
        );
        // A packed bin and a cleared lane: each surface its own.
        let i = road.cell_index(500.0);
        for v in &mut road.snow[i * BINS..(i + 1) * BINS] {
            *v = 0.25;
        }
        road.ice[i * BINS..(i + 1) * BINS].fill(0.5);
        let s = road.sample(500.0, 2.0);
        assert!((s.snow - 0.25).abs() < 1e-6 && (s.ice - 0.5).abs() < 1e-6);
        assert_eq!((s.paved_snow, s.verge_snow), (1.0, 1.0));
        road.lane_snow.fill(0.1);
        if !road.lane_snow.is_empty() {
            let lane = road.sample_in_lane(500.0, 2.0, 5.0);
            assert!((lane.paved_snow - 0.1).abs() < 1e-6);
        }
        assert_eq!(
            RoadState::line_snow_of(&TrackConfig {
                road_state: None,
                ..track.clone()
            }),
            1.0
        );
        // Off a snow session: nothing.
        track = dipped_track(0.5);
        let wet = RoadState::new(&track, RUBBER_REFERENCE);
        let s = wet.sample(500.0, 2.0);
        assert_eq!([s.snow, s.ice, s.paved_snow, s.verge_snow], [0.0; 4]);
        track.road_state = Some(wet);
        assert_eq!(RoadState::line_snow_of(&track), 0.0);
    }

    #[test]
    fn the_cars_pack_a_line_that_grips_half_again_and_the_snow_keeps_falling_beside_it() {
        let mut road = RoadState::new(&snowy_track(-3.0), RUBBER_REFERENCE);
        // Twenty cars, twenty laps in half an hour of snow.
        drive(&mut road, 2.0, 400, 1800.0);
        let line = road.sample(500.0, 2.8).grip;
        let beside = road.sample(500.0, -5.0).grip;
        println!("snow after 400 passes: line {line:.3}, beside it {beside:.3}");
        assert!(line > 1.3 && line < 1.6, "a driven line: {line}");
        assert!((beside - 1.0).abs() < 0.03, "untouched snow: {beside}");
        assert!(road.mean_line_snow() < 0.8 && road.mean_line_snow() > SNOW_FLOOR - 0.01);
        // The line is a snowy bin: rubber lays on it far slower than on a
        // dry one.
        let mut dry = RoadState::new(&dipped_track(0.0), RUBBER_REFERENCE);
        drive(&mut dry, 2.0, 400, 1800.0);
        let k = road.cell_index(500.0) * BINS + bin_of(2.8).unwrap();
        assert!(
            road.rubber[k] < dry.rubber[k] - 0.05,
            "{} against {}",
            road.rubber[k],
            dry.rubber[k]
        );
    }

    /// The pit lane carries a cover of its own on its own stations: the cars
    /// in it pack it where they drive, the run-off beside the lap keeps the
    /// verge's, and a lane nobody drives stays as the snowfall leaves it.
    #[test]
    fn the_cars_in_the_pit_lane_pack_its_own_cover() {
        let mut track = snowy_track(-3.0);
        track.pit_lane = Some(crate::pit::tests::straight_lane());
        let mut road = RoadState::new(&track, RUBBER_REFERENCE);
        assert_eq!(road.lane_snow.len(), 20, "200 m of lane in 10 m cells");
        assert_eq!(road.lane_snow_at(95.0), Some(1.0));
        // As baked at the start, in the lane as on the run-off.
        let lane = road.sample_in_lane(500.0, 20.0, 95.0);
        assert!((lane.wet_grip - 1.0).abs() < 1e-6, "{}", lane.wet_grip);
        // The snow has stopped on a freezing track; twenty cars an hour
        // through the lane's first half, for an hour. (Under a snowfall
        // the lane's few cars barely keep up with it.)
        road.snowfall = 0.0;
        for t in 0..3600 {
            if t % 180 == 0 {
                for station in (0..10).map(|c| 5.0 + 10.0 * c as f32) {
                    road.note_lane_pass(station, 4.0);
                }
            }
            road.step(1.0, 1.0);
        }
        let (driven, untouched) = (
            road.lane_snow_at(55.0).unwrap(),
            road.lane_snow_at(155.0).unwrap(),
        );
        println!("lane cover after an hour: driven {driven:.3}, untouched {untouched:.3}");
        assert!((SNOW_FLOOR..0.9).contains(&driven), "{driven}");
        assert!(untouched > 0.99, "{untouched}");
        let packed = road.sample_in_lane(500.0, 20.0, 55.0).wet_grip;
        let verge = road.sample(500.0, 20.0).wet_grip;
        assert!(
            packed > verge + 0.1,
            "lane {packed} against run-off {verge}"
        );
        assert_eq!(
            road.sample_in_lane(500.0, 20.0, 155.0).wet_grip,
            verge,
            "an undriven cell is the verge's cover"
        );
        // On the wire: a byte a lane cell, the driven half packed below the rest.
        let mut bytes = Vec::new();
        road.write_lane_snow(&mut bytes);
        assert_eq!(bytes.len(), 20);
        assert!(bytes[5] < bytes[15], "{} against {}", bytes[5], bytes[15]);
        assert_eq!(bytes[15], 255);
        // A track without a lane reads the verge, as before.
        let plain = RoadState::new(&snowy_track(-3.0), RUBBER_REFERENCE);
        assert!(plain.lane_snow.is_empty());
        let mut none = Vec::new();
        plain.write_lane_snow(&mut none);
        assert!(none.is_empty(), "nothing on the wire without a lane");
        assert_eq!(
            plain.sample_in_lane(500.0, 20.0, 55.0),
            plain.sample(500.0, 20.0)
        );
    }

    #[test]
    fn a_dry_road_under_new_snow_loses_grip_to_the_covered_figure() {
        let mut road = RoadState::new(&dipped_track(0.0), RUBBER_REFERENCE);
        road.track_c = -3.0;
        assert_eq!(road.sample(500.0, 0.0).grip, 1.0);
        road.snowfall = 1.0;
        for _ in 0..900 {
            road.step(1.0, 1.0);
        }
        let s = road.sample(500.0, 0.0);
        assert!(road.has_snow());
        // The covered figure: the slush is the cover's own.
        assert!((s.grip - 0.28).abs() < 0.01, "{}", s.grip);
        assert!(
            s.wet_grip < 0.3,
            "the run-off is buried too: {}",
            s.wet_grip
        );
        assert!(road.mean_line_snow() > 0.95);
        assert!(road.sample(500.0, 12.0).off_grip < 0.5, "the verge too");
        // The snow stops; a track over freezing melts it away.
        road.snowfall = 0.0;
        road.track_c = 3.0;
        for _ in 0..1800 {
            road.step(1.0, 1.0);
        }
        assert!(road.mean_line_snow() < 0.01, "{}", road.mean_line_snow());
        assert!(road.sample(500.0, 0.0).grip > 0.99, "bare again");
        assert!(
            !road.has_snow(),
            "nothing left: the sample is neutral again"
        );
    }

    /// A snow session's road melted clear grips as asphalt does: wet while
    /// the melt water stands, dry once it has drained, the covered bake's
    /// 0.28 back to 1.0. The run-off and the pit lane melt with the verge.
    #[test]
    fn a_road_melted_clear_grips_as_asphalt_and_the_run_off_clears_with_it() {
        let mut road = RoadState::new(&snowy_track(-3.0), RUBBER_REFERENCE);
        let covered = Weather::Snow.road_grip_factor();
        let start = road.sample(500.0, 0.0);
        assert!((start.wet_grip - 1.0).abs() < 1e-6, "{}", start.wet_grip);
        assert!((start.off_grip - 1.0).abs() < 1e-6, "{}", start.off_grip);
        road.snowfall = 0.0;
        road.track_c = 8.0;
        let mut wet_seen = false;
        for t in 0..7200 {
            road.step(1.0, 1.0);
            let s = road.sample(500.0, 0.0);
            if t == 400 {
                // Melted, the melt water still on it: wet asphalt.
                let absolute = s.grip * covered;
                let asphalt = road_grip_for(s.water);
                println!(
                    "melted at 400 s: water {:.2}, grip {absolute:.3} (asphalt {asphalt:.3})",
                    s.water
                );
                wet_seen = s.water > 0.05;
                assert!(road.mean_line_snow() < 0.01);
                assert!((absolute - asphalt).abs() < 0.01, "{absolute} vs {asphalt}");
            }
        }
        let s = road.sample(500.0, 0.0);
        let absolute = s.grip * covered;
        println!(
            "drained: water {:.3}, grip {absolute:.3}, run-off {:.3}",
            s.water,
            s.wet_grip * covered
        );
        assert!(wet_seen, "the melt leaves water a while");
        assert!(s.water < 0.01, "{}", s.water);
        assert!((absolute - 1.0).abs() < 0.01, "bone dry: {absolute}");
        assert!(
            (s.wet_grip * covered - 1.0).abs() < 0.01,
            "the run-off: {}",
            s.wet_grip
        );
        assert!(s.off_grip > 3.0, "the grass is bare again: {}", s.off_grip);
    }

    #[test]
    fn the_water_on_a_cleared_line_freezes_on_a_cold_track_and_thaws_after() {
        let mut road = RoadState::new(&snowy_track(-6.0), RUBBER_REFERENCE);
        drive(&mut road, 2.0, 100, 900.0);
        // The snow stops falling overnight; the line stays cold and wet.
        road.snowfall = 0.0;
        road.rain = 0.2;
        drive(&mut road, 2.0, 60, 1800.0);
        let k = road.cell_index(500.0) * BINS + bin_of(2.8).unwrap();
        let ice = road.ice[k];
        let line = road.sample(500.0, 2.8).grip;
        println!("frozen: ice {ice:.2}, line grip {line:.2}");
        assert!(ice > 0.5, "the line froze: {ice}");
        assert!(line < 1.2, "ice takes the packed line's advantage: {line}");
        // The sun comes up and the track climbs over freezing.
        road.track_c = 4.0;
        for _ in 0..1800 {
            road.step(1.0, 1.0);
        }
        assert!(road.ice[k] < 0.05, "thawed: {}", road.ice[k]);
    }

    #[test]
    fn write_snow_rows_packs_the_verge_then_the_bins_cover_and_ice() {
        let mut road = RoadState::new(&snowy_track(-3.0), RUBBER_REFERENCE);
        drive(&mut road, 2.0, 100, 900.0);
        let n = road.cell_count();
        let first = n - 1;
        let mut bytes = Vec::new();
        road.write_snow_rows(first, 2, &mut bytes);
        assert_eq!(bytes.len(), 2 * SNOW_ROW_BYTES);
        assert_eq!(bytes[0], 255, "the verge is white");
        let b = bin_of(2.8).unwrap();
        let k = first * BINS + b;
        assert_eq!(
            bytes[1 + 2 * b],
            (road.snow[k] * 255.0).round().clamp(0.0, 255.0) as u8
        );
        assert!(bytes[1 + 2 * b] < 255, "the line is packed");
        assert_eq!(bytes[1 + 2 * b + 1], 0, "and not frozen");
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
        assert_eq!(
            green.sample(500.0, 2.0).grip,
            green.sample(500.0, -5.0).grip
        );
    }

    #[test]
    fn write_rows_packs_the_cells_the_wire_reads() {
        let mut road = RoadState::new(&dipped_track(0.5), 1.0);
        drive(&mut road, 2.0, 200, 1800.0);
        let n = road.cell_count();
        // A wrapping slice of three cells from near the end of the lap.
        let first = n - 2;
        let mut bytes = Vec::new();
        road.write_rows(first, 3, &mut bytes);
        assert_eq!(bytes.len(), 3 * ROW_BYTES);
        for k in 0..3 {
            let i = (first + k) % n;
            let off = k * ROW_BYTES;
            let cell = &road.cells[i];
            assert_eq!(
                bytes[off],
                (cell.depth * DEPTH_BYTE).round().clamp(0.0, 255.0) as u8,
                "cell {i} depth byte",
            );
            let base = i * BINS;
            // A bin on the raceline carries the rubber the laps laid.
            let b = bin_of(2.0).unwrap();
            let rubber_byte = bytes[off + 1 + 3 * b];
            assert_eq!(
                rubber_byte,
                (road.rubber[base + b] * 255.0).round().clamp(0.0, 255.0) as u8,
            );
        }
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
    fn a_wheel_counts_by_its_load_and_lays_rubber_by_its_sliding() {
        let weight = 12_000.0;
        let mut work = RoadWork::default();
        // Cornering left: the right wheels carry most of it, the inside
        // front is in the air; the outside front slides hardest.
        work.add(
            0.1,
            [0.0, 7000.0, 500.0, 3500.0],
            [0.0, 7000.0, 1000.0, 6000.0],
            [0.0, 0.004, 0.0, 0.002],
        );
        let w = pass_weights(&work, weight);
        assert_eq!(w[0], 0.0, "a lifted wheel lays nothing");
        assert!(w[1] > 1.9 && w[3] > 1.5 && w[2] < 0.5, "{w:?}");
        assert!((w.iter().sum::<f32>() - 4.0).abs() < 1e-4);
        // Downforce presses the passes no harder than the weight does.
        let mut fast = RoadWork::default();
        fast.add(0.1, [0.0; 4], [6000.0; 4], [0.0; 4]);
        assert_eq!(pass_weights(&fast, weight), [1.0; 4]);
        // A car crawling on half its weight's load counts half.
        let mut light = RoadWork::default();
        light.add(0.1, [0.0; 4], [1500.0; 4], [0.0; 4]);
        assert_eq!(pass_weights(&light, weight), [0.5; 4]);
        assert_eq!(pass_weights(&RoadWork::default(), weight), [0.0; 4]);
        let passes = work.passes(weight);
        assert_eq!(passes[0].rubber, 0.0);
        assert!((passes[1].rubber - 700.0 / RUBBER_SLIDE_J).abs() < 1e-4);
        assert!((work.shed() - 0.006 / MARBLE_WEAR_PCT).abs() < 1e-4);
        assert_eq!(shed_for_wear(0.0), 0.0, "rolling straight sheds nothing");
        work.restart();
        assert_eq!(work.seconds, 0.0);
        assert_eq!(work.wheels[1], WheelRoadWork::default());
    }

    #[test]
    fn a_line_driven_sliding_rubbers_in_and_one_rolled_only_dries() {
        let mut road = RoadState::new(&dipped_track(0.0), RUBBER_REFERENCE);
        let n = road.cells.len();
        for _ in 0..200 {
            for i in 0..n {
                let pass = |lateral_m: f32, rubber: f32| WheelPass {
                    ahead_m: 0.0,
                    lateral_m,
                    weight: 1.0,
                    rubber,
                };
                road.note_pass(
                    i as f32 * CELL_M + 1.0,
                    &[pass(-5.0, 0.0), pass(5.0, 3.0)],
                    0.0,
                );
            }
            road.step(1.0, 1.0);
        }
        let rubber_at = |road: &RoadState, lateral: f32| {
            road.rubber[road.cell_index(500.0) * BINS + bin_of(lateral).unwrap()]
        };
        let rolled = rubber_at(&road, -5.0);
        let slid = rubber_at(&road, 5.0);
        println!("200 passes: rolled {rolled:.3}, slid {slid:.3}");
        assert!((rolled - RUBBER_REFERENCE).abs() < 1e-4, "{rolled}");
        assert!(slid > RUBBER_REFERENCE + 0.3, "{slid}");
        // No marbles shed, none lying.
        assert!(road.marbles.iter().all(|m| *m == 0.0));
    }

    #[test]
    fn a_wheel_lays_its_pass_in_the_cell_its_patch_is_in() {
        let mut road = RoadState::new(&dipped_track(0.5), RUBBER_REFERENCE);
        let front = WheelPass {
            ahead_m: 6.0,
            ..WheelPass::plain(3.0)
        };
        for _ in 0..20 {
            road.note_pass(505.0, &[front], 0.0);
            road.step(1.0, 1.0);
        }
        let i = road.cell_index(505.0);
        let b = bin_of(3.0).unwrap();
        assert_eq!(road.dry[i * BINS + b], 0.0, "the car's own cell");
        assert!(road.dry[(i + 1) * BINS + b] > 0.3, "the patch's");
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
        assert!((flat - 0.5).abs() < 0.02, "{flat}");
        assert!(road.puddle_at(1000.0) > 0.5 && road.puddle_at(200.0) < 0.05);
        // The dip's puddle lies across part of the road: deep in it, the
        // flat road's water beside it.
        let cell = &road.cells[road.cell_index(1000.0)];
        let (at, half) = (cell.puddle_lateral_m, cell.puddle_half_m);
        assert!(half > 1.5, "{half}");
        let dip = road.water_at(1000.0, at);
        assert!(dip > 0.6, "a puddle: {dip}");
        let beside = if at > 0.0 {
            at - half - 1.0
        } else {
            at + half + 1.0
        };
        let dry_side = road.water_at(1000.0, beside);
        assert!(
            (dry_side - 0.5).abs() < 0.03,
            "beside the puddle: {dry_side}"
        );
        // The wire carries it per bin.
        let mut bins = Vec::new();
        road.write_water_rows(road.cell_index(1000.0), 1, &mut bins);
        assert_eq!(bins.len(), BINS);
        let peak = bins.iter().copied().max().unwrap();
        assert!(peak as f32 > 0.6 * DEPTH_BYTE, "{bins:?}");
        assert!(road.has_puddles());
        // The baked water is the reference: the flat grips as baked.
        assert!((road.sample(200.0, 0.0).grip / rubber_grip(0.0, 0.5) - 1.0).abs() < 0.03);
        // Off the lap's end it wraps.
        assert!((road.water_at(2000.0 + 200.0, 0.0) - flat).abs() < 0.03);
    }

    #[test]
    fn a_cambered_corner_holds_water_along_its_low_edge() {
        // The dipped track flattened, banked 4.5 degrees to the right
        // (positive lowers the right) from 400 m to 600 m.
        let mut track = dipped_track(0.5);
        for p in track.centerline.iter_mut() {
            p.z = 10.0;
            if (400.0..600.0).contains(&p.distance_from_start_m) {
                p.banking_rad = 0.08;
            }
        }
        let road = RoadState::new(&track, RUBBER_REFERENCE);
        assert_eq!(edge_low(0.0), 0.0);
        assert_eq!(edge_low(-0.08), EDGE_LOW_MAX);
        assert!(edge_low(0.02) > 0.0 && edge_low(0.02) < EDGE_LOW_MAX);
        let cell = &road.cells[road.cell_index(500.0)];
        assert_eq!(cell.low, EDGE_LOW_MAX);
        assert!(cell.puddle_lateral_m > 5.0, "the right edge: {cell:?}");
        let edge = road.water_at(500.0, 7.5);
        let high = road.water_at(500.0, -6.0);
        let flat = road.water_at(200.0, 7.5);
        println!("banked: low edge {edge:.3}, high side {high:.3}; flat road {flat:.3}");
        assert!(edge > 0.58, "a stream along the low edge: {edge}");
        assert!((high - 0.5).abs() < 0.03, "the high side drains: {high}");
        assert!((flat - 0.5).abs() < 0.02, "{flat}");
        assert!(road.has_puddles());
        assert_eq!(road.puddle_cells(), 0, "an edge is not a dip");
        // In the dry nothing stands anywhere.
        let mut dry = track.clone();
        dry.track_surface.water = 0.0;
        dry.track_surface.baked_water = 0.0;
        let dry = RoadState::new(&dry, RUBBER_REFERENCE);
        assert_eq!(dry.water_at(500.0, 7.5), 0.0);
        assert_eq!(dry.sample(500.0, 7.5).grip, 1.0);
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
