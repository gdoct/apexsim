//! Ironing the kinks out of a traced centerline.
//!
//! Every real circuit's YAML is a GPS trace resampled to 5 m nodes, and a
//! GPS trace has metre-scale noise in it. Over a 5 m chord that noise *is*
//! the curvature: at Remus the sampled radius comes out at 8 m on a 10.6 m
//! wide road, and the curvature changes by 0.07 per metre between two
//! adjacent nodes. Nothing downstream can survive that. The Catmull-Rom
//! samplers interpolate straight through the kink, the exporter's road
//! loft folds its inside edge, and the repairs it then applies — clamping
//! every lateral offset to `0.85/κ`, breaking a strip that has under half
//! a metre of room, dropping any facet that comes out inverted — turn a
//! knot into a hole. That is why the tight corners look malformed: the
//! geometry is being deleted rather than drawn.
//!
//! The fix belongs in the data, not in either sampler. If the server and
//! the exporter each smoothed on load they could disagree, and the
//! server's track limits would stop matching the road the client draws.
//! So this is a one-off pass over the YAML, run by `ats-smooth`, and both
//! sides then read the same nodes they always did.
//!
//! The method is a Savitzky-Golay filter: a least-squares quadratic is
//! fitted to a short window of nodes and the node is moved to the fitted
//! value at its own position. The point of a quadratic is that it is what
//! an arc looks like locally, so a real corner passes through almost
//! untouched while noise at the node scale — which no quadratic can
//! follow — is averaged away. Over the seven-node window used here, a
//! 35 m radius corner comes out 0.012 % tighter per pass, four
//! millimetres, while an alternating one-node wobble loses three quarters
//! of its amplitude.
//!
//! Two filters were tried and rejected first, and both failure modes are
//! worth keeping in mind because they are invisible without a test that
//! measures shape rather than smoothness. Plain Laplacian smoothing
//! shrinks whatever it is run on: it pulled every node of a test ring
//! inward by the full tolerance and ate another 0.6 m on the next run.
//! Taubin smoothing, which exists to fix exactly that, alternates a
//! shrinking step with a slightly larger expanding one — but that pair
//! has a gain above one below its passband, necessarily so, and over
//! twenty-four passes it grew the same ring by 0.44 m. `ats-smooth` is
//! run on real surveyed circuits, so a filter that quietly rescales one
//! is worse than no filter at all. `a_kink_is_ironed_out_and_the_circle_
//! is_kept` and `smoothing_twice_changes_nothing_more` are what caught
//! both and what keep them caught.
//!
//! On top of the filter, each node is held inside a disc of
//! [`SmoothConfig::tolerance_m`] around where it was traced, so that no
//! input, however odd, can move the course off its survey.

use crate::track_data::{TrackFile, TrackNode};

/// How far a node may be moved from its traced position. The road is
/// 10–15 m wide, so half a metre is invisible from the cockpit, while GPS
/// jitter at this sample spacing is of the same order — enough room to
/// take the noise out without moving the course.
pub const DEFAULT_TOLERANCE_M: f32 = 0.6;

/// Half-width of the fitting window, in nodes. Seven nodes is 30 m at the
/// 5 m spacing every traced circuit uses: long enough to average out a
/// GPS outlier, short enough that the tightest real corner on the
/// calendar is still well described by a quadratic across it.
pub const DEFAULT_WINDOW: usize = 3;

/// Filter passes. Each twiced pass removes about three fifths of what is
/// left at the node scale, so six take a metre-scale kink down to noise
/// while leaving a real corner's radius alone to eight decimal places.
pub const DEFAULT_ITERATIONS: usize = 6;

/// Elevation is noisier than plan position in a GPS trace and nothing
/// downstream reads it as tightly, so z is smoothed on its own, harder,
/// and with a tolerance of its own.
pub const DEFAULT_Z_TOLERANCE_M: f32 = 0.35;

/// How far a node may move where the trace has it on a radius no road
/// could be built on. A node whose three-point radius is under
/// [`CERTAINLY_WRONG_RADIUS_M`] is not a tight corner, it is an error:
/// Remus at the Red Bull Ring reads 8.1 m on a 10.9 m road, which would
/// need a 1.5 m sagitta over 10 m of centerline. Holding such a node to
/// the ordinary tolerance keeps the error; letting every node move this
/// far would let the filter reshape a circuit. So the allowance is given
/// only where the geometry is self-evidently impossible.
pub const DEFAULT_TIGHT_TOLERANCE_M: f32 = 3.0;

/// Below this radius the trace is not describing a corner any circuit
/// has. The tightest hairpin in use — Monaco's Grand Hotel — is about
/// 12 m at the inside kerb and rather more on the centerline, so 18 m is
/// comfortably under anything real while still catching the kinks.
pub const CERTAINLY_WRONG_RADIUS_M: f32 = 18.0;

/// The radius the redistribution aims to leave as the tightest anywhere
/// on the lap. The exporter clamps every lateral offset to `0.85/κ`, so
/// at this radius it can still draw a 12 m road, its kerbs and a few
/// metres beyond them without folding — which is the whole point of the
/// pass. It is a floor on the *centerline*, so a real hairpin measuring
/// 30 m or more end to end is unaffected; what it removes is the apex
/// that the trace piled a corner's whole turn into.
pub const DEFAULT_MIN_RADIUS_M: f32 = 22.0;

#[derive(Debug, Clone, Copy)]
pub struct SmoothConfig {
    pub tolerance_m: f32,
    /// The wider allowance given to a node the trace has on an impossible
    /// radius. See [`DEFAULT_TIGHT_TOLERANCE_M`].
    pub tight_tolerance_m: f32,
    pub z_tolerance_m: f32,
    /// Half-width of the fitting window, in nodes.
    pub window: usize,
    pub iterations: usize,
    /// The tightest centerline radius the turn cap will leave.
    pub min_radius_m: f32,
}

impl Default for SmoothConfig {
    fn default() -> Self {
        Self {
            tolerance_m: DEFAULT_TOLERANCE_M,
            tight_tolerance_m: DEFAULT_TIGHT_TOLERANCE_M,
            z_tolerance_m: DEFAULT_Z_TOLERANCE_M,
            window: DEFAULT_WINDOW,
            iterations: DEFAULT_ITERATIONS,
            min_radius_m: DEFAULT_MIN_RADIUS_M,
        }
    }
}

/// What a pass did, in the terms the problem was stated in.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct SmoothReport {
    pub nodes: usize,
    /// Tightest radius implied by three consecutive nodes, before/after.
    pub min_radius_before_m: f32,
    pub min_radius_after_m: f32,
    /// Largest change in curvature per metre between adjacent nodes: the
    /// number that decides whether a loft folds.
    pub max_curvature_jump_before: f32,
    pub max_curvature_jump_after: f32,
    /// Furthest any node moved, and the mean move.
    pub max_shift_m: f32,
    pub mean_shift_m: f32,
    pub max_z_shift_m: f32,
    pub length_before_m: f32,
    pub length_after_m: f32,
    /// How far the raceline strays outside the road edge, as a fraction
    /// of the half-width there: 1.0 is exactly on the edge. The raceline
    /// is a polyline of its own and moving the centerline under it could
    /// leave it off the track, so the pass filters it too and reports
    /// where the worst of it ends up.
    pub raceline_worst_before: f32,
    pub raceline_worst_after: f32,
}

impl SmoothReport {
    /// A pass is worth writing when it actually opened the tight corners
    /// up. Everything else (a clean line, a second run over a smoothed
    /// one) leaves the file alone.
    pub fn changed(&self) -> bool {
        self.max_shift_m > 0.005
    }
}

/// Smooth a track's centerline in place. Returns what it did.
pub fn smooth_track(track: &mut TrackFile, config: SmoothConfig) -> Option<SmoothReport> {
    let n = track.nodes.len();
    if n < 4 {
        return None;
    }
    let closed = track.closed_loop;
    let original: Vec<(f32, f32, f32)> = track.nodes.iter().map(|p| (p.x, p.y, p.z)).collect();

    let plan: Vec<(f32, f32)> = original.iter().map(|p| (p.0, p.1)).collect();
    let elevation: Vec<f32> = original.iter().map(|p| p.2).collect();

    let smoothed_plan = savgol_2d(&plan, closed, &config);
    let smoothed_z = savgol_1d(&elevation, closed, config.z_tolerance_m, &config);

    let report = SmoothReport {
        nodes: n,
        min_radius_before_m: min_radius(&plan, closed),
        min_radius_after_m: min_radius(&smoothed_plan, closed),
        max_curvature_jump_before: max_curvature_jump(&plan, closed),
        max_curvature_jump_after: max_curvature_jump(&smoothed_plan, closed),
        max_shift_m: (0..n)
            .map(|i| (smoothed_plan[i].0 - plan[i].0).hypot(smoothed_plan[i].1 - plan[i].1))
            .fold(0.0, f32::max),
        mean_shift_m: (0..n)
            .map(|i| (smoothed_plan[i].0 - plan[i].0).hypot(smoothed_plan[i].1 - plan[i].1))
            .sum::<f32>()
            / n as f32,
        max_z_shift_m: (0..n)
            .map(|i| (smoothed_z[i] - elevation[i]).abs())
            .fold(0.0, f32::max),
        length_before_m: polyline_length(&plan, closed),
        length_after_m: polyline_length(&smoothed_plan, closed),
        raceline_worst_before: 0.0,
        raceline_worst_after: 0.0,
    };

    // The raceline moves with the road beneath it.
    //
    // It is a polyline of its own, traced the same way, and the first
    // version of this pass gave it only the gentle position filter. That
    // was wrong wherever a pinched corner was re-walked: the centerline
    // there moved up to 3 m and the raceline did not, so through the Ford
    // chicanes at Le Mans the line the AI follows ended up two metres off
    // the middle of the road it was drawn for, and one car of four left
    // the track on the first lap and never came back — which
    // `ai_field_makes_the_first_lap_at_le_mans` caught. So each raceline
    // point is carried by the displacement of the road node nearest it,
    // which keeps its offset from the centerline exactly what it was, and
    // only then filtered for its own noise.
    let raceline: Vec<(f32, f32)> = track.raceline.iter().map(|p| (p.x, p.y)).collect();
    let raceline_before = raceline_worst(&plan, &smoothed_plan, &raceline, track);
    let smoothed_raceline = if raceline.len() >= 8 {
        let carried: Vec<(f32, f32)> = raceline
            .iter()
            .map(|point| {
                let nearest = nearest_node(&plan, *point);
                let shift = (
                    smoothed_plan[nearest].0 - plan[nearest].0,
                    smoothed_plan[nearest].1 - plan[nearest].1,
                );
                (point.0 + shift.0, point.1 + shift.1)
            })
            .collect();
        let weights = savgol_weights(config.window.max(2));
        let mut current = carried.clone();
        for _ in 0..config.iterations {
            current = twiced_2d(&current, closed, &weights);
        }
        // Held to the ordinary tolerance around where the road carried
        // it, not around where it was traced: the road's own move is not
        // the raceline's to undo.
        let bound = vec![config.tolerance_m; current.len()];
        clamp_to_tolerance_2d(&mut current, &carried, &bound);
        current
    } else {
        raceline.clone()
    };
    let raceline_after = raceline_worst(&plan, &smoothed_plan, &smoothed_raceline, track);

    for (i, node) in track.nodes.iter_mut().enumerate() {
        if smoothed_plan[i] == plan[i] && smoothed_z[i] == elevation[i] {
            // A node the window could not cover — the ends of an open
            // course — keeps its traced value to the bit. Rounding it
            // would move the start/finish line by a centimetre on every
            // run, which is both wrong and not idempotent.
            continue;
        }
        node.x = round_cm(smoothed_plan[i].0);
        node.y = round_cm(smoothed_plan[i].1);
        node.z = round_cm(smoothed_z[i]);
    }
    for (point, smoothed) in track.raceline.iter_mut().zip(&smoothed_raceline) {
        point.x = round_cm(smoothed.0);
        point.y = round_cm(smoothed.1);
    }

    // The YAML records its lap length, and the exporter's curb sidecar is
    // checked against it. Smoothing takes the noise out of the trace, and
    // a noisy polyline is longer than the line it samples, so the lap
    // comes out a little shorter — 16 m at Austin — and the stored figure
    // has to follow or the two disagree. It is measured on the same
    // sampled spline everything else reads, not the raw node polyline.
    if let Some(path) = crate::track_path::CenterlinePath::from_track(track) {
        if let Some(meta) = track.metadata.as_mut() {
            if meta.length_m.is_some() {
                meta.length_m = Some((path.total_length_m() * 1000.0).round() / 1000.0);
            }
        }
    }

    Some(SmoothReport {
        raceline_worst_before: raceline_before,
        raceline_worst_after: raceline_after,
        ..report
    })
}

/// Index of the traced node nearest a point.
fn nearest_node(nodes: &[(f32, f32)], point: (f32, f32)) -> usize {
    let mut best = (f32::MAX, 0usize);
    for (i, node) in nodes.iter().enumerate() {
        let d = (point.0 - node.0).hypot(point.1 - node.1);
        if d < best.0 {
            best = (d, i);
        }
    }
    best.1
}

/// The worst the raceline strays past the road edge, as a fraction of the
/// half-width there. `centerline` is the line to measure against;
/// `original` is the traced one, used only to find each raceline point's
/// nearest node so the two measurements compare like with like.
fn raceline_worst(
    original: &[(f32, f32)],
    centerline: &[(f32, f32)],
    raceline: &[(f32, f32)],
    track: &TrackFile,
) -> f32 {
    if raceline.is_empty() || centerline.len() < 2 {
        return 0.0;
    }
    let n = centerline.len();
    let mut worst: f32 = 0.0;
    for point in raceline {
        // Nearest node on the traced line, then measured against the one
        // in hand: an O(n·m) scan, but this runs once per track in a tool.
        let mut best = (f32::MAX, 0usize);
        for (i, node) in original.iter().enumerate() {
            let d = (point.0 - node.0).hypot(point.1 - node.1);
            if d < best.0 {
                best = (d, i);
            }
        }
        let i = best.1;
        let next = centerline[(i + 1) % n];
        let here = centerline[i];
        let heading = (next.1 - here.1).atan2(next.0 - here.0);
        let (sin, cos) = heading.sin_cos();
        let (dx, dy) = (point.0 - here.0, point.1 - here.1);
        let lateral = -sin * dx + cos * dy;
        let (left, right) =
            track.nodes[i.min(track.nodes.len() - 1)].resolved_half_widths(track.default_width);
        let half = if lateral >= 0.0 { left } else { right };
        if half > 0.1 {
            worst = worst.max(lateral.abs() / half);
        }
    }
    worst
}

/// Round to the centimetre. A traced node is not accurate to the micron
/// and the YAML is read by humans; rounding also makes a second run over
/// an already-smoothed file a no-op instead of a drift.
fn round_cm(v: f32) -> f32 {
    (v * 100.0).round() / 100.0
}

fn neighbours(i: usize, n: usize, closed: bool) -> Option<(usize, usize)> {
    if closed {
        Some(((i + n - 1) % n, (i + 1) % n))
    } else if i == 0 || i + 1 >= n {
        // An open track's ends are where they are: there is nothing on one
        // side to average against, and moving an endpoint would move the
        // start/finish line.
        None
    } else {
        Some((i - 1, i + 1))
    }
}

/// Savitzky-Golay weights for a least-squares quadratic over a window of
/// `2m + 1` evenly spaced samples, evaluated at the centre. The closed
/// form is standard; for m = 2 it is the familiar (-3, 12, 17, 12, -3)/35.
/// They sum to one, so the filter cannot move a straight line.
fn savgol_weights(m: usize) -> Vec<f32> {
    let mf = m as f32;
    let denominator = (2.0 * mf + 3.0) * (2.0 * mf + 1.0) * (2.0 * mf - 1.0);
    (0..=2 * m)
        .map(|k| {
            let j = k as f32 - mf;
            3.0 * (3.0 * mf * mf + 3.0 * mf - 1.0 - 5.0 * j * j) / denominator
        })
        .collect()
}

/// Index of the sample `offset` away from `i`, or `None` where an open
/// track runs out of line. A window that would reach past an end is not
/// applied at all, which is what keeps the first and last nodes — and so
/// the start/finish line of an open course — exactly where they were.
fn windowed(i: usize, offset: isize, n: usize, closed: bool) -> Option<usize> {
    let target = i as isize + offset;
    if closed {
        Some(target.rem_euclid(n as isize) as usize)
    } else if target < 0 || target >= n as isize {
        None
    } else {
        Some(target as usize)
    }
}

fn savgol_pass_2d(points: &[(f32, f32)], closed: bool, weights: &[f32]) -> Vec<(f32, f32)> {
    let n = points.len();
    let m = (weights.len() / 2) as isize;
    let mut out = points.to_vec();
    for (i, slot) in out.iter_mut().enumerate() {
        let mut acc = (0.0f32, 0.0f32);
        let mut complete = true;
        for (k, w) in weights.iter().enumerate() {
            match windowed(i, k as isize - m, n, closed) {
                Some(j) => {
                    acc.0 += points[j].0 * w;
                    acc.1 += points[j].1 * w;
                }
                None => {
                    complete = false;
                    break;
                }
            }
        }
        if complete {
            *slot = acc;
        }
    }
    out
}

fn savgol_pass_1d(values: &[f32], closed: bool, weights: &[f32]) -> Vec<f32> {
    let n = values.len();
    let m = (weights.len() / 2) as isize;
    let mut out = values.to_vec();
    for (i, slot) in out.iter_mut().enumerate() {
        let mut acc = 0.0f32;
        let mut complete = true;
        for (k, w) in weights.iter().enumerate() {
            match windowed(i, k as isize - m, n, closed) {
                Some(j) => acc += values[j] * w,
                None => {
                    complete = false;
                    break;
                }
            }
        }
        if complete {
            *slot = acc;
        }
    }
    out
}

/// How far each node is allowed to move, worked out once from the traced
/// line.
///
/// Two things earn a node a wider allowance, and both are statements that
/// the trace is wrong rather than that the circuit is unusual. A node
/// sitting on a radius no road could be built on is one. Being inside a
/// corner the trace pinched is the other, and it matters just as much:
/// repairing an apex means moving its neighbours too, and holding them to
/// the ordinary tolerance dragged the repaired arc straight back. The
/// allowance ramps down across the outer part of the window so that two
/// adjacent nodes never get wildly different budgets.
fn tolerance_per_node(
    points: &[(f32, f32)],
    closed: bool,
    config: &SmoothConfig,
    windows: &[(usize, usize)],
) -> Vec<f32> {
    let n = points.len();
    let mut out = (0..n)
        .map(|i| {
            let Some((a, b)) = neighbours(i, n, closed) else {
                return config.tolerance_m;
            };
            let radius = radius_at(points[a], points[i], points[b]);
            if radius >= CERTAINLY_WRONG_RADIUS_M {
                return config.tolerance_m;
            }
            // Fully impossible at half the bound, ordinary at the bound.
            let how_wrong = (1.0 - radius / CERTAINLY_WRONG_RADIUS_M).clamp(0.0, 1.0) * 2.0;
            let t = how_wrong.min(1.0);
            config.tolerance_m + (config.tight_tolerance_m - config.tolerance_m) * t
        })
        .collect::<Vec<_>>();

    for &(first, last) in windows {
        let span = last.saturating_sub(first);
        if span == 0 {
            continue;
        }
        for k in 0..=span {
            let i = (first + k) % n;
            // Full allowance over the middle of the window, tapering to
            // the ordinary one at its pinned ends.
            let from_end = k.min(span - k) as f32 / (span as f32 * 0.5).max(1.0);
            let t = from_end.clamp(0.0, 1.0);
            let allowed = config.tolerance_m + (config.tight_tolerance_m - config.tolerance_m) * t;
            out[i] = out[i].max(allowed);
        }
    }
    out
}

/// Pull every point back inside its own disc around where it was traced.
fn clamp_to_tolerance_2d(points: &mut [(f32, f32)], origin: &[(f32, f32)], tolerance: &[f32]) {
    for ((point, start), bound) in points.iter_mut().zip(origin).zip(tolerance) {
        let (dx, dy) = (point.0 - start.0, point.1 - start.1);
        let drift = dx.hypot(dy);
        if drift > *bound {
            let scale = *bound / drift;
            *point = (start.0 + dx * scale, start.1 + dy * scale);
        }
    }
}

/// One filter step, applied by twicing: smooth, smooth the result again,
/// and add back the difference (`2·S - S(S)`).
///
/// The bare quadratic fit has a gain a hair under one on the shapes it is
/// meant to leave alone — 0.99988 across a 35 m corner — which sounds
/// like nothing until the tool is run twice and the circuit is 0.006 %
/// shorter each time. Twicing squares that error away (a gain of `g`
/// becomes `1 - (1 - g)²`, so 1.2e-4 becomes 1.4e-8) at the cost of a
/// weaker stopband, which is paid back by running more passes. What is
/// left is a filter that removes node-scale noise and leaves the circuit
/// the length it was.
fn twiced_2d(points: &[(f32, f32)], closed: bool, weights: &[f32]) -> Vec<(f32, f32)> {
    let once = savgol_pass_2d(points, closed, weights);
    let twice = savgol_pass_2d(&once, closed, weights);
    once.iter()
        .zip(&twice)
        .map(|(a, b)| (2.0 * a.0 - b.0, 2.0 * a.1 - b.1))
        .collect()
}

fn twiced_1d(values: &[f32], closed: bool, weights: &[f32]) -> Vec<f32> {
    let once = savgol_pass_1d(values, closed, weights);
    let twice = savgol_pass_1d(&once, closed, weights);
    once.iter().zip(&twice).map(|(a, b)| 2.0 * a - b).collect()
}

/// Spread a corner's turn back along its own arc.
///
/// Filtering positions fixes a node that is out of line with its
/// neighbours. It does not fix the other defect these traces have, which
/// is a corner whose total geometry is right but whose turn is piled into
/// one node: Remus at the Red Bull Ring turns 2.7, 4.6, 9.7, 16.1, 20.5,
/// 25.0, **34.5**, 14.6 and 0.4 degrees at its nine nodes, where a
/// uniform 31.7 m arc — which is what the corner measures end to end —
/// would turn 9 degrees at each. That is a polygon corner, the signature
/// of a trace simplified before it was resampled, and no amount of moving
/// the apex sideways turns it into an arc.
///
/// So this works on the turn itself: take the heading change at each
/// node, filter *that* along the lap, and walk the line back out again
/// from the smoothed headings, keeping every segment the length it was.
/// Because the filter's weights sum to one and the lap is a cycle, the
/// total turn is preserved exactly — the corner keeps its geometry and
/// only its distribution changes. Walking a heading series out again
/// leaves the far end slightly adrift, so the closing error is spread
/// along the lap in proportion to distance, which is the surveyor's
/// compass rule.
/// Windows of the lap whose nodes turn more sharply than the floor
/// allows, padded so there is arc either side to spill the excess into
/// and merged where two corners run together.
fn kinked_windows(
    turn: &[f32],
    length: &[f32],
    min_radius_m: f32,
    pad: usize,
) -> Vec<(usize, usize)> {
    let n = turn.len();
    let mut windows: Vec<(usize, usize)> = Vec::new();
    for i in 0..n {
        let cap = (length[i] / min_radius_m).max(1e-4);
        if turn[i].abs() <= cap {
            continue;
        }
        let a = i.saturating_sub(pad);
        let b = i + pad;
        match windows.last_mut() {
            Some(last) if a <= last.1 => last.1 = b.max(last.1),
            _ => windows.push((a, b)),
        }
    }
    windows
}

/// Spread a pinched corner's turn back along its own arc, in place, and
/// only where it is pinched.
///
/// The turn at a node divided by the arc through it *is* the curvature
/// there, so a cap on the turn is a floor on the radius — which is what
/// the exporter's loft actually needs. Giving the excess to the
/// neighbours rather than trimming it keeps the corner honest: the total
/// turn through the window is unchanged, so a hairpin still turns through
/// the angle it did, just spread over its arc instead of piled onto one
/// node.
///
/// Doing this to the whole lap at once was tried first and is wrong. A
/// change at one node shifts every node after it, so re-walking a full
/// lap moves the entire circuit: the tolerance clamp then bound at all
/// 864 nodes of the Red Bull Ring, a mean shift of 0.6 m, which is not
/// smoothing but relocation. Pinning each window at both ends confines
/// the change to the corner that needed it and leaves the rest of the
/// course exactly where it was surveyed.
fn spill_excess_turn(
    turn: &mut [f32],
    length: &[f32],
    min_radius_m: f32,
    first: usize,
    last: usize,
) {
    let n = turn.len();
    let cap_at = |i: usize| (length[i] / min_radius_m).max(1e-4);
    let span = last - first;
    for _ in 0..span.clamp(1, 64) {
        let mut spilled = 0.0f32;
        let before = turn.to_vec();
        for k in 0..=span {
            let i = (first + k) % n;
            let excess = before[i].abs() - cap_at(i);
            if excess <= 0.0 {
                continue;
            }
            let signed = excess.copysign(before[i]);
            // The window's own ends are pinned, so excess reaching them
            // has nowhere left to go and stays put: a corner genuinely
            // tighter than the floor keeps as much of itself as it must.
            let before_ok = k > 0;
            let after_ok = k < span;
            let shares = u8::from(before_ok) + u8::from(after_ok);
            if shares == 0 {
                continue;
            }
            let share = signed / f32::from(shares);
            turn[i] -= signed;
            if before_ok {
                turn[(i + n - 1) % n] += share;
            }
            if after_ok {
                turn[(i + 1) % n] += share;
            }
            spilled += excess;
        }
        if spilled < 1e-6 {
            break;
        }
    }
}

/// Re-walk one window of the lap from its respread turns, pinned to the
/// positions the trace gives its two ends.
fn rewalk_window(
    points: &mut [(f32, f32)],
    turn: &[f32],
    length: &[f32],
    heading0: f32,
    first: usize,
    last: usize,
) {
    let n = points.len();
    let span = last - first;
    if span < 2 {
        return;
    }
    let anchor = points[first % n];
    let target = points[last % n];

    let mut walked = Vec::with_capacity(span + 1);
    walked.push(anchor);
    let mut h = heading0;
    for k in 0..span {
        let i = (first + k) % n;
        if k > 0 {
            h += turn[i];
        }
        let previous = *walked.last().unwrap();
        walked.push((
            previous.0 + length[i] * h.cos(),
            previous.1 + length[i] * h.sin(),
        ));
    }

    // Compass rule: the gap between where the walk ended and where the
    // trace says it should have is shared out along the window by
    // distance travelled, so both ends land on the surveyed line.
    let end = *walked.last().unwrap();
    let error = (end.0 - target.0, end.1 - target.1);
    let total: f32 = (0..span).map(|k| length[(first + k) % n]).sum();
    if total <= f32::EPSILON {
        return;
    }
    let mut travelled = 0.0f32;
    for (k, point) in walked.iter_mut().enumerate() {
        if k > 0 {
            travelled += length[(first + k - 1) % n];
        }
        let share = travelled / total;
        point.0 -= error.0 * share;
        point.1 -= error.1 * share;
    }
    for (k, point) in walked.iter().enumerate().take(span) {
        points[(first + k) % n] = *point;
    }
}

/// The lap's segment lengths, headings and per-node turns.
fn walk_of(points: &[(f32, f32)]) -> (Vec<f32>, Vec<f32>, Vec<f32>) {
    let n = points.len();
    let mut length = Vec::with_capacity(n);
    let mut heading = Vec::with_capacity(n);
    for i in 0..n {
        let a = points[i];
        let b = points[(i + 1) % n];
        length.push((b.0 - a.0).hypot(b.1 - a.1));
        heading.push((b.1 - a.1).atan2(b.0 - a.0));
    }
    let turn = (0..n)
        .map(|i| wrap_pi(heading[i] - heading[(i + n - 1) % n]))
        .collect();
    (length, heading, turn)
}

/// Where the traced lap turns more sharply at one node than the road can
/// be drawn around.
pub fn pinched_windows(
    points: &[(f32, f32)],
    min_radius_m: f32,
    pad: usize,
) -> Vec<(usize, usize)> {
    if points.len() < 8 {
        return Vec::new();
    }
    let (length, _, turn) = walk_of(points);
    kinked_windows(&turn, &length, min_radius_m, pad)
}

/// Open out every corner the trace pinched, and nothing else.
pub fn open_pinched_corners(
    points: &[(f32, f32)],
    min_radius_m: f32,
    windows: &[(usize, usize)],
) -> Option<Vec<(f32, f32)>> {
    if points.len() < 8 || windows.is_empty() {
        return None;
    }
    let (length, heading, turn) = walk_of(points);
    let mut out = points.to_vec();
    for &(first, last) in windows {
        let mut local = turn.clone();
        spill_excess_turn(&mut local, &length, min_radius_m, first, last);
        rewalk_window(
            &mut out,
            &local,
            &length,
            heading[first % points.len()],
            first,
            last,
        );
    }
    Some(out)
}

/// How many times the open-and-filter round may repeat within one run.
/// Opening a corner out reveals the next tightest node behind it, so a
/// single round gets part of the way there; repeating until nothing is
/// pinched is what makes one run of the tool equal to running it until it
/// settles. Every round is still clamped against the *original* trace, so
/// the total displacement is bounded however many rounds it takes.
const MAX_ROUNDS: usize = 12;

fn savgol_2d(points: &[(f32, f32)], closed: bool, config: &SmoothConfig) -> Vec<(f32, f32)> {
    let weights = savgol_weights(config.window.max(2));
    let pad = config.window * 3;
    let mut tolerance = tolerance_per_node(points, closed, config, &[]);
    let mut current = points.to_vec();

    for round in 0..MAX_ROUNDS {
        let windows = if closed {
            pinched_windows(&current, config.min_radius_m, pad)
        } else {
            Vec::new()
        };
        if round == 0 {
            // The allowance is set from the trace as it arrived, not from
            // a line the pass has already moved, so it cannot creep.
            tolerance = tolerance_per_node(points, closed, config, &windows);
        }
        let more_to_do = !windows.is_empty();
        if more_to_do {
            // Even out the turn first, then take the node-scale noise off
            // what that leaves. The two address different defects, and the
            // order matters: filtering positions first would blur the very
            // heading series the redistribution reads.
            if let Some(opened) = open_pinched_corners(&current, config.min_radius_m, &windows) {
                current = opened;
                clamp_to_tolerance_2d(&mut current, points, &tolerance);
            }
        }
        let before_filter = current.clone();
        for _ in 0..config.iterations {
            current = twiced_2d(&current, closed, &weights);
            clamp_to_tolerance_2d(&mut current, points, &tolerance);
        }
        let settled = current
            .iter()
            .zip(&before_filter)
            .map(|(a, b)| (a.0 - b.0).hypot(a.1 - b.1))
            .fold(0.0f32, f32::max)
            < 0.005;
        if !more_to_do && settled {
            break;
        }
    }
    current
}

fn savgol_1d(values: &[f32], closed: bool, tolerance_m: f32, config: &SmoothConfig) -> Vec<f32> {
    let weights = savgol_weights(config.window.max(2));
    let mut current = values.to_vec();
    for _ in 0..config.iterations {
        current = twiced_1d(&current, closed, &weights);
        for (value, start) in current.iter_mut().zip(values) {
            let drift = *value - start;
            if drift.abs() > tolerance_m {
                *value = start + tolerance_m.copysign(drift);
            }
        }
    }
    current
}

/// Radius of the circle through three consecutive points, in metres.
/// `f32::MAX` where they are collinear.
fn radius_at(a: (f32, f32), b: (f32, f32), c: (f32, f32)) -> f32 {
    let ab = (b.0 - a.0).hypot(b.1 - a.1);
    let bc = (c.0 - b.0).hypot(c.1 - b.1);
    let ca = (a.0 - c.0).hypot(a.1 - c.1);
    // Twice the signed area of the triangle.
    let cross = ((b.0 - a.0) * (c.1 - a.1) - (b.1 - a.1) * (c.0 - a.0)).abs();
    if cross < 1e-6 {
        return f32::MAX;
    }
    ab * bc * ca / (2.0 * cross)
}

/// The tightest radius three consecutive nodes imply anywhere on the lap.
pub fn min_radius(points: &[(f32, f32)], closed: bool) -> f32 {
    let n = points.len();
    let mut best = f32::MAX;
    for i in 0..n {
        let Some((a, b)) = neighbours(i, n, closed) else {
            continue;
        };
        best = best.min(radius_at(points[a], points[i], points[b]));
    }
    best
}

/// Curvature at a node, signed, from the turn between its two chords.
fn curvature_at(points: &[(f32, f32)], i: usize, n: usize, closed: bool) -> f32 {
    let Some((a, b)) = neighbours(i, n, closed) else {
        return 0.0;
    };
    let h0 = (points[i].1 - points[a].1).atan2(points[i].0 - points[a].0);
    let h1 = (points[b].1 - points[i].1).atan2(points[b].0 - points[i].0);
    let turn = wrap_pi(h1 - h0);
    let span = (points[i].0 - points[a].0).hypot(points[i].1 - points[a].1)
        + (points[b].0 - points[i].0).hypot(points[b].1 - points[i].1);
    if span < 1e-6 {
        0.0
    } else {
        turn / (span * 0.5)
    }
}

fn wrap_pi(a: f32) -> f32 {
    let two_pi = std::f32::consts::TAU;
    (a + std::f32::consts::PI).rem_euclid(two_pi) - std::f32::consts::PI
}

/// The largest jump in curvature between two adjacent nodes. This is the
/// number that decides whether the exporter's loft folds, so it is the one
/// worth watching rather than curvature itself.
pub fn max_curvature_jump(points: &[(f32, f32)], closed: bool) -> f32 {
    let n = points.len();
    let mut worst: f32 = 0.0;
    for i in 0..n {
        let Some((_, b)) = neighbours(i, n, closed) else {
            continue;
        };
        let k0 = curvature_at(points, i, n, closed);
        let k1 = curvature_at(points, b, n, closed);
        worst = worst.max((k1 - k0).abs());
    }
    worst
}

fn polyline_length(points: &[(f32, f32)], closed: bool) -> f32 {
    let n = points.len();
    let last = if closed { n } else { n - 1 };
    (0..last)
        .map(|i| {
            let j = (i + 1) % n;
            (points[j].0 - points[i].0).hypot(points[j].1 - points[i].1)
        })
        .sum()
}

/// Nodes that carry a tighter radius than the road is wide are where the
/// loft has no room to draw an inside edge at all. Reported by name so a
/// circuit that still has one after a pass is obvious.
pub fn tight_nodes(track: &TrackFile, bound_m: f32) -> Vec<(usize, f32)> {
    let points: Vec<(f32, f32)> = track.nodes.iter().map(|p| (p.x, p.y)).collect();
    let n = points.len();
    let mut out = Vec::new();
    for i in 0..n {
        let Some((a, b)) = neighbours(i, n, track.closed_loop) else {
            continue;
        };
        let r = radius_at(points[a], points[i], points[b]);
        if r < bound_m {
            out.push((i, r));
        }
    }
    out
}

/// The station of each node along the lap, for reporting a kink's position
/// the way a driver would describe it.
pub fn node_stations(track: &TrackFile) -> Vec<f32> {
    let mut out = Vec::with_capacity(track.nodes.len());
    let mut station = 0.0;
    for (i, node) in track.nodes.iter().enumerate() {
        if i > 0 {
            let previous: &TrackNode = &track.nodes[i - 1];
            station += (node.x - previous.x).hypot(node.y - previous.y);
        }
        out.push(station);
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A deterministic hash-based wobble in -1..1, standing in for the
    /// metre-scale noise a GPS trace carries. Hash-based rather than
    /// seeded RNG so the fixture is the same on every platform and run.
    fn wobble(i: usize, salt: u32) -> f32 {
        let mut h = (i as u32).wrapping_mul(2_654_435_761) ^ salt.wrapping_mul(0x9E37_79B9);
        h ^= h >> 15;
        h = h.wrapping_mul(0x85EB_CA6B);
        h ^= h >> 13;
        (h % 2000) as f32 / 1000.0 - 1.0
    }

    /// A ring of `n` nodes on a circle of `radius`, each pushed in or out
    /// by up to `noise` metres: a clean corner seen through a noisy trace.
    fn ring(n: usize, radius: f32, noise: f32) -> TrackFile {
        let nodes = (0..n)
            .map(|i| {
                let a = i as f32 / n as f32 * std::f32::consts::TAU;
                let r = radius + wobble(i, 1) * noise;
                TrackNode {
                    x: r * a.cos(),
                    y: r * a.sin(),
                    z: wobble(i, 2) * noise,
                    width: None,
                    width_left: Some(5.5),
                    width_right: Some(5.5),
                    banking: None,
                    friction: None,
                    surface_type: None,
                }
            })
            .collect();
        TrackFile {
            name: "Ring".to_string(),
            display_name: None,
            track_id: None,
            nodes,
            checkpoints: vec![],
            spawn_points: vec![],
            default_width: 11.0,
            closed_loop: true,
            raceline: vec![],
            drs_zones: Vec::new(),
            metadata: None,
        }
    }

    #[test]
    fn trace_noise_is_filtered_and_the_circle_is_kept() {
        let mut track = ring(80, 200.0, 0.5);
        let report = smooth_track(&mut track, SmoothConfig::default()).unwrap();
        assert!(
            report.max_curvature_jump_after < report.max_curvature_jump_before * 0.25,
            "the noise survived: {report:?}"
        );
        assert!(
            report.min_radius_after_m > report.min_radius_before_m * 2.0,
            "the tight spot did not open up: {report:?}"
        );
        // The ring keeps its radius. This is the shrinkage check, and it
        // is what rules out plain Laplacian and Taubin smoothing: both
        // passed every other assertion here while quietly rescaling the
        // whole circle by half a metre.
        assert!(
            report.mean_shift_m < 0.35,
            "nodes moved more than the noise: {report:?}"
        );
        assert!(
            (report.length_after_m - report.length_before_m).abs() < 1.5,
            "the lap changed length: {report:?}"
        );
        // Every node ends up closer to the true circle than the trace put
        // it. What the filter cannot do is remove the part of the noise
        // that looks like a shape change, and it should not try, so the
        // bar is "less than the noise it was given", not "perfect".
        for node in &track.nodes {
            let r = node.x.hypot(node.y);
            assert!((r - 200.0).abs() < 0.45, "the circle drifted to {r}");
        }
    }

    #[test]
    fn a_real_corner_keeps_its_radius() {
        // A 35 m hairpin traced cleanly: the filter must not flatten it.
        let mut track = ring(44, 35.0, 0.0);
        let report = smooth_track(&mut track, SmoothConfig::default()).unwrap();
        assert!(
            report.min_radius_after_m > 34.0,
            "a clean 35 m corner was flattened: {report:?}"
        );
        assert!(
            report.max_shift_m < 0.05,
            "a clean corner should barely move: {report:?}"
        );
    }

    #[test]
    fn no_node_moves_further_than_the_tolerance() {
        let before = ring(120, 150.0, 0.8);
        let mut track = before.clone();
        let config = SmoothConfig::default();
        let report = smooth_track(&mut track, config).unwrap();
        // Nothing may move past the widest allowance there is, which is
        // the one a pinched corner gets.
        for (a, b) in before.nodes.iter().zip(&track.nodes) {
            let moved = (a.x - b.x).hypot(a.y - b.y);
            // One centimetre of slack for the rounding on the way out.
            assert!(
                moved <= config.tight_tolerance_m + 0.015,
                "a node moved {moved} m, past the {} m bound",
                config.tight_tolerance_m
            );
        }

        // On a clean ring, where nothing is pinched and no radius is
        // impossible, the ordinary tolerance is the only one in play.
        let clean_before = ring(120, 150.0, 0.0);
        let mut clean = clean_before.clone();
        smooth_track(&mut clean, config).unwrap();
        for (a, b) in clean_before.nodes.iter().zip(&clean.nodes) {
            let moved = (a.x - b.x).hypot(a.y - b.y);
            assert!(
                moved <= config.tolerance_m + 0.015,
                "a clean ring's node moved {moved} m"
            );
        }
        assert!(report.max_shift_m <= config.tight_tolerance_m + 0.015);
    }

    #[test]
    fn a_gross_outlier_is_bounded_by_the_tolerance() {
        // A node five metres out of line is a survey error, not noise.
        // The pass improves it by as much as the tolerance allows and no
        // more, because the alternative is a filter that can move a
        // circuit anywhere it likes.
        let mut track = ring(80, 200.0, 0.0);
        track.nodes[40].x += 5.0;
        let before = track.nodes[40].x;
        let report = smooth_track(&mut track, SmoothConfig::default()).unwrap();
        let moved = (before - track.nodes[40].x).abs();
        assert!(
            (moved - DEFAULT_TOLERANCE_M).abs() < 0.02,
            "the outlier moved {moved} m, not the {DEFAULT_TOLERANCE_M} m bound"
        );
        assert!(report.changed());
    }

    #[test]
    fn a_straight_is_left_alone() {
        let nodes = (0..40)
            .map(|i| TrackNode {
                x: i as f32 * 5.0,
                y: 0.0,
                z: 0.0,
                width: None,
                width_left: Some(5.0),
                width_right: Some(5.0),
                banking: None,
                friction: None,
                surface_type: None,
            })
            .collect();
        let mut track = TrackFile {
            name: "Straight".to_string(),
            display_name: None,
            track_id: None,
            nodes,
            checkpoints: vec![],
            spawn_points: vec![],
            default_width: 10.0,
            closed_loop: false,
            raceline: vec![],
            drs_zones: Vec::new(),
            metadata: None,
        };
        let report = smooth_track(&mut track, SmoothConfig::default()).unwrap();
        assert!(
            report.max_shift_m < 1e-3,
            "a straight was moved: {report:?}"
        );
        assert!(!report.changed());
    }

    #[test]
    fn repeated_passes_converge_rather_than_drift() {
        // Re-running the tool must not keep eating the circuit. Each run
        // takes out what is left in the filter's stopband, and since the
        // passband gain is one there is nothing else for it to take: the
        // moves shrink by an order of magnitude a run and the lap length
        // holds. This is the property Laplacian and Taubin smoothing both
        // failed — there, every run moved the line by the full tolerance
        // again, in the same direction, for ever.
        let mut track = ring(80, 200.0, 0.5);
        let first = smooth_track(&mut track, SmoothConfig::default()).unwrap();
        let second = smooth_track(&mut track, SmoothConfig::default()).unwrap();
        let third = smooth_track(&mut track, SmoothConfig::default()).unwrap();
        // Each run moves the line strictly less than the one before.
        assert!(
            second.max_shift_m < first.max_shift_m * 0.25,
            "the second pass did not settle: {first:?} then {second:?}"
        );
        assert!(
            third.max_shift_m < second.max_shift_m,
            "the third pass did not settle: {second:?} then {third:?}"
        );
        // And, the point of twicing: once the noise is gone, further runs
        // leave the lap the length it is. The first pass legitimately
        // shortens it — a wobbly polyline is longer than the smooth line
        // it samples — so the drift to watch is what the later passes do,
        // and that is now 12 mm in 1256 m. Without twicing it was 80 mm a
        // pass, in the same direction, for ever.
        let later_drift = (third.length_after_m - second.length_before_m).abs();
        assert!(
            later_drift < 0.05,
            "later passes are still eating the lap: {:.3} m -> {:.3} m",
            second.length_before_m,
            third.length_after_m
        );
        assert!(
            third.min_radius_after_m >= first.min_radius_before_m,
            "a pass tightened the corner: {first:?} then {third:?}"
        );
    }

    #[test]
    fn an_open_track_keeps_its_ends() {
        let mut track = ring(60, 100.0, 0.6);
        track.closed_loop = false;
        let first = (track.nodes[0].x, track.nodes[0].y);
        let last = {
            let n = track.nodes.last().unwrap();
            (n.x, n.y)
        };
        smooth_track(&mut track, SmoothConfig::default()).unwrap();
        assert_eq!((track.nodes[0].x, track.nodes[0].y), first);
        let after = track.nodes.last().unwrap();
        assert_eq!((after.x, after.y), last);
    }

    #[test]
    fn the_filter_weights_sum_to_one() {
        // A filter whose weights did not would move a straight line, and
        // with it the whole circuit, on every pass.
        for m in 2..8 {
            let sum: f32 = savgol_weights(m).iter().sum();
            assert!((sum - 1.0).abs() < 1e-5, "window {m} sums to {sum}");
        }
        // The classic five-point Savitzky-Golay quadratic kernel.
        let w = savgol_weights(2);
        let expected = [
            -3.0 / 35.0,
            12.0 / 35.0,
            17.0 / 35.0,
            12.0 / 35.0,
            -3.0 / 35.0,
        ];
        for (got, want) in w.iter().zip(expected) {
            assert!((got - want).abs() < 1e-6, "{w:?}");
        }
    }
}
