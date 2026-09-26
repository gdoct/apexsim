//! Put a circuit's banking where its corners really are, leaning the way
//! they really lean.
//!
//! The real circuits' banking came from `enrich_all_tracks.py`, which laid
//! each banked corner as a window a tenth of a lap wide around a guessed
//! *fraction of the lap*, always with a positive angle. Two things follow
//! from that and both showed at Zandvoort:
//!
//! * **The sign ignores the direction of the bend.** Positive banking lifts
//!   the *left* edge (`track_path::offset_point`, the server's
//!   `surface_elevation`), which is right for a right-hander and exactly
//!   inverted for a left-hander — the Hugenholtzbocht, a left-hander, was
//!   drawn leaning out of the corner.
//! * **The window is not the corner.** A fraction of the lap drifts from
//!   the corner by however far the guess was off: the Arie Luyendijkbocht's
//!   18° started on the main straight after the corner was over, and the
//!   Hugenholtz banking ran on 200 m over the Hunserug.
//!
//! This pass keeps what the data says about *how much* each corner is
//! banked and re-derives *where* and *which way* from the centerline:
//! every banked span is matched to the bend it was meant for (the one it
//! overlaps with the most turning, else the nearest one), and the angle is
//! re-laid over that bend's own extent, signed to lower its inside edge,
//! with a short ramp either side. Banking left on a straight with no bend
//! near it is removed.

use crate::track_data::TrackFile;
use crate::track_smooth::node_stations;

/// A node counts as banked above this, radians (about 0.3°).
const BANKED_RAD: f32 = 0.005;
/// Half-length of the chord the curvature is measured over, metres. The
/// nodes are 5 m apart and a trace's noise is curvature over one chord.
const KAPPA_HALF_CHORD_M: f32 = 15.0;
/// A station is in a bend where the curvature exceeds this (1/300 m):
/// the Arie Luyendijkbocht is ~130 m radius and must count.
const BEND_KAPPA: f32 = 1.0 / 300.0;
/// Bends of the same hand closer than this are one corner.
const BEND_MERGE_GAP_M: f32 = 30.0;
/// Within a matched bend the banking holds where the curvature is at least
/// this share of the bend's peak; the rest is the ramp.
const BEND_CORE_SHARE: f32 = 0.35;
/// Ramp in and out of a banked bend, metres.
const RAMP_M: f32 = 30.0;
/// A span that overlaps no bend is given to the nearest one only when it
/// is at most this far away, metres.
const MAX_MATCH_GAP_M: f32 = 250.0;

/// What [`realign_banking`] did to one track.
#[derive(Debug, Default, Clone, PartialEq)]
pub struct BankReport {
    /// Banked spans found in the file as it was.
    pub spans: usize,
    /// Bends banked after the pass: (from, to, angle rad — signed).
    pub bends: Vec<(f32, f32, f32)>,
    /// Spans dropped for having no bend near them: (from, to).
    pub dropped: Vec<(f32, f32)>,
    /// Largest per-node change, radians.
    pub max_change_rad: f32,
}

impl BankReport {
    pub fn changed(&self) -> bool {
        self.max_change_rad > 1e-4
    }
}

#[derive(Debug, Clone, Copy)]
struct Bend {
    from_m: f32,
    to_m: f32,
    /// +1 left-hander, -1 right-hander.
    hand: f32,
    peak_kappa: f32,
}

/// Signed curvature at every node (positive = left-hand bend), from the
/// heading change over a chord either side of it.
fn node_curvature(track: &TrackFile, stations: &[f32]) -> Vec<f32> {
    let n = track.nodes.len();
    let total = total_length(track, stations);
    let closed = track.closed_loop;
    let at = |i: isize| -> (f32, f32) {
        let idx = if closed {
            i.rem_euclid(n as isize) as usize
        } else {
            i.clamp(0, n as isize - 1) as usize
        };
        (track.nodes[idx].x, track.nodes[idx].y)
    };
    let spacing = (total / n as f32).max(0.5);
    let k = ((KAPPA_HALF_CHORD_M / spacing).round() as isize).max(1);
    (0..n as isize)
        .map(|i| {
            let (a, b, c) = (at(i - k), at(i), at(i + k));
            let h1 = (b.1 - a.1).atan2(b.0 - a.0);
            let h2 = (c.1 - b.1).atan2(c.0 - b.0);
            let mut dh = h2 - h1;
            while dh > std::f32::consts::PI {
                dh -= std::f32::consts::TAU;
            }
            while dh < -std::f32::consts::PI {
                dh += std::f32::consts::TAU;
            }
            let arc = (b.0 - a.0).hypot(b.1 - a.1) + (c.0 - b.0).hypot(c.1 - b.1);
            if arc > 1e-3 {
                dh / (arc / 2.0)
            } else {
                0.0
            }
        })
        .collect()
}

fn total_length(track: &TrackFile, stations: &[f32]) -> f32 {
    let last = *stations.last().unwrap_or(&0.0);
    if track.closed_loop && track.nodes.len() > 1 {
        let (a, b) = (&track.nodes[track.nodes.len() - 1], &track.nodes[0]);
        last + (b.x - a.x).hypot(b.y - a.y)
    } else {
        last
    }
}

/// Contiguous node ranges over a predicate, as (first, last) inclusive
/// indices; on a closed loop a range running through node 0 is joined.
fn runs(n: usize, closed: bool, on: impl Fn(usize) -> bool) -> Vec<(usize, usize)> {
    let mut out: Vec<(usize, usize)> = Vec::new();
    let mut i = 0;
    while i < n {
        if !on(i) {
            i += 1;
            continue;
        }
        let start = i;
        while i < n && on(i) {
            i += 1;
        }
        out.push((start, i - 1));
    }
    if closed && out.len() >= 2 && out[0].0 == 0 && out.last().unwrap().1 == n - 1 {
        let first = out.remove(0);
        out.last_mut().unwrap().1 = first.1 + n;
    }
    out
}

/// Station of node index `i`, where `i` may run past the end of a closed
/// loop (a range joined through start/finish).
fn station_of(i: usize, stations: &[f32], total: f32) -> f32 {
    let n = stations.len();
    stations[i % n] + (i / n) as f32 * total
}

fn bends(kappa: &[f32], stations: &[f32], total: f32, closed: bool) -> Vec<Bend> {
    let n = kappa.len();
    let mut out: Vec<Bend> = Vec::new();
    for (a, b) in runs(n, closed, |i| kappa[i].abs() >= BEND_KAPPA) {
        // Split a run where the hand changes: a chicane is two bends.
        let mut s = a;
        for i in a..=b {
            let flip = i < b && kappa[(i + 1) % n].signum() != kappa[s % n].signum();
            if flip || i == b {
                let peak = (s..=i).map(|j| kappa[j % n].abs()).fold(0.0, f32::max);
                out.push(Bend {
                    from_m: station_of(s, stations, total),
                    to_m: station_of(i, stations, total),
                    hand: kappa[s % n].signum(),
                    peak_kappa: peak,
                });
                s = i + 1;
            }
        }
    }
    let mut merged: Vec<Bend> = Vec::new();
    for bend in out {
        match merged.last_mut() {
            Some(prev) if prev.hand == bend.hand && bend.from_m - prev.to_m < BEND_MERGE_GAP_M => {
                prev.to_m = bend.to_m;
                prev.peak_kappa = prev.peak_kappa.max(bend.peak_kappa);
            }
            _ => merged.push(bend),
        }
    }
    merged
}

/// Signed distance from `from` forward to `to` round a closed loop.
fn ahead(from: f32, to: f32, total: f32, closed: bool) -> f32 {
    if closed {
        (to - from).rem_euclid(total)
    } else {
        to - from
    }
}

/// Re-lay the track's banking over the bends it belongs to. See the module
/// docs. `None` when the track is too short to have a curvature.
pub fn realign_banking(track: &mut TrackFile) -> Option<BankReport> {
    let n = track.nodes.len();
    if n < 8 {
        return None;
    }
    let closed = track.closed_loop;
    let stations = node_stations(track);
    let total = total_length(track, &stations);
    let kappa = node_curvature(track, &stations);
    let bank: Vec<f32> = track
        .nodes
        .iter()
        .map(|nd| nd.banking.unwrap_or(0.0))
        .collect();
    let spans = runs(n, closed, |i| bank[i].abs() > BANKED_RAD);
    let bend_list = bends(&kappa, &stations, total, closed);

    let mut report = BankReport {
        spans: spans.len(),
        ..Default::default()
    };
    // Angle per bend (magnitude), from whichever spans claim it.
    let mut claimed: Vec<f32> = vec![0.0; bend_list.len()];
    for &(a, b) in &spans {
        let magnitude = (a..=b).map(|i| bank[i % n].abs()).fold(0.0, f32::max);
        let (from, to) = (
            station_of(a, &stations, total),
            station_of(b, &stations, total),
        );
        // Turning inside the overlap with each bend: a crest the span
        // spills over does not outvote the corner it was meant for.
        let mut best: Option<(usize, f32)> = None;
        for (bi, bend) in bend_list.iter().enumerate() {
            let turning: f32 = (a..=b)
                .filter(|&i| {
                    let s = station_of(i, &stations, total);
                    let d = ahead(bend.from_m, s, total, closed);
                    d <= bend.to_m - bend.from_m
                })
                .map(|i| kappa[i % n].abs())
                .sum();
            if turning > 0.0 && best.is_none_or(|(_, t)| turning > t) {
                best = Some((bi, turning));
            }
        }
        let pick = best.map(|(bi, _)| bi).or_else(|| {
            bend_list
                .iter()
                .enumerate()
                .map(|(bi, bend)| {
                    let gap = ahead(bend.to_m, from, total, closed).min(ahead(
                        to,
                        bend.from_m,
                        total,
                        closed,
                    ));
                    (bi, gap)
                })
                .filter(|&(_, gap)| gap <= MAX_MATCH_GAP_M)
                .min_by(|x, y| x.1.total_cmp(&y.1))
                .map(|(bi, _)| bi)
        });
        match pick {
            Some(bi) => claimed[bi] = claimed[bi].max(magnitude),
            None => report.dropped.push((from, to)),
        }
    }

    let mut new_bank = vec![0.0f32; n];
    for (bi, bend) in bend_list.iter().enumerate() {
        let magnitude = claimed[bi];
        if magnitude <= 0.0 {
            continue;
        }
        // The core: where this bend turns hardest. Positive banking lifts
        // the left edge, which is what a right-hander (negative
        // curvature) wants, so the sign is against the hand.
        let angle = -bend.hand * magnitude;
        let span = bend.to_m - bend.from_m;
        let in_bend = |s: f32| {
            let d = ahead(bend.from_m, s, total, closed);
            (d <= span).then_some(d)
        };
        let core: Vec<f32> = (0..n)
            .filter_map(|i| {
                let d = in_bend(stations[i])?;
                (kappa[i].abs() >= BEND_CORE_SHARE * bend.peak_kappa && kappa[i] * bend.hand > 0.0)
                    .then_some(d)
            })
            .collect();
        let (c0, c1) = match (
            core.iter().copied().reduce(f32::min),
            core.iter().copied().reduce(f32::max),
        ) {
            (Some(lo), Some(hi)) => (bend.from_m + lo, bend.from_m + hi),
            _ => (bend.from_m, bend.to_m),
        };
        for (i, value) in new_bank.iter_mut().enumerate() {
            let s = stations[i];
            // Distance outside the core, either way round.
            let before = ahead(s, c0, total, closed);
            let after = ahead(c1, s, total, closed);
            let inside = ahead(c0, s, total, closed) <= c1 - c0;
            let w = if inside {
                1.0
            } else {
                let gap = before.min(after);
                if gap >= RAMP_M {
                    0.0
                } else {
                    let t = 1.0 - gap / RAMP_M;
                    t * t * (3.0 - 2.0 * t)
                }
            };
            if w > 0.0 && (angle * w).abs() > value.abs() {
                *value = angle * w;
            }
        }
        report.bends.push((
            c0.rem_euclid(if closed { total } else { f32::MAX }),
            c1.rem_euclid(if closed { total } else { f32::MAX }),
            angle,
        ));
    }

    for (node, value) in track.nodes.iter_mut().zip(&new_bank) {
        let old = node.banking.unwrap_or(0.0);
        report.max_change_rad = report.max_change_rad.max((old - value).abs());
        let rounded = (value * 10_000.0).round() / 10_000.0;
        // Keep the file's own shape: a node that had no banking field and
        // still has none keeps it absent.
        if node.banking.is_some() || rounded != 0.0 {
            node.banking = Some(rounded);
        }
    }
    Some(report)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::track_data::{TrackFile, TrackNode};

    fn node(x: f32, y: f32, banking: f32) -> TrackNode {
        TrackNode {
            x,
            y,
            z: 0.0,
            width: None,
            width_left: Some(5.0),
            width_right: Some(5.0),
            banking: Some(banking),
            friction: None,
            surface_type: None,
        }
    }

    /// A stadium: two 300 m straights and two 60 m-radius half circles,
    /// run anticlockwise (every bend a left-hander), 5 m nodes.
    fn stadium(bank: impl Fn(f32) -> f32) -> TrackFile {
        let r = 60.0;
        let straight = 300.0;
        let mut pts: Vec<(f32, f32)> = Vec::new();
        let mut s = 0.0;
        while s < straight {
            pts.push((s, -r));
            s += 5.0;
        }
        let steps = (std::f32::consts::PI * r / 5.0) as usize;
        for i in 0..steps {
            let a = -std::f32::consts::FRAC_PI_2 + i as f32 / steps as f32 * std::f32::consts::PI;
            pts.push((straight + r * a.cos(), r * a.sin()));
        }
        let mut s = straight;
        while s > 0.0 {
            pts.push((s, r));
            s -= 5.0;
        }
        for i in 0..steps {
            let a = std::f32::consts::FRAC_PI_2 + i as f32 / steps as f32 * std::f32::consts::PI;
            pts.push((r * a.cos(), r * a.sin()));
        }
        let mut station = 0.0;
        let mut prev = pts[0];
        let nodes = pts
            .iter()
            .map(|&(x, y)| {
                station += (x - prev.0).hypot(y - prev.1);
                prev = (x, y);
                node(x, y, bank(station))
            })
            .collect();
        TrackFile {
            name: "stadium".into(),
            display_name: None,
            track_id: None,
            nodes,
            checkpoints: vec![],
            spawn_points: vec![],
            default_width: 10.0,
            closed_loop: true,
            raceline: vec![],
            drs_zones: vec![],
            metadata: None,
        }
    }

    fn bank_at(track: &TrackFile, x: f32, y: f32) -> f32 {
        track
            .nodes
            .iter()
            .min_by(|a, b| {
                (a.x - x)
                    .hypot(a.y - y)
                    .total_cmp(&(b.x - x).hypot(b.y - y))
            })
            .unwrap()
            .banking
            .unwrap()
    }

    #[test]
    fn a_left_hander_is_banked_with_its_left_edge_low() {
        // Authored positive (left edge high) over the first bend.
        let mut track = stadium(|s| {
            if (300.0..490.0).contains(&s) {
                0.3
            } else {
                0.0
            }
        });
        let report = realign_banking(&mut track).unwrap();
        assert!(report.changed());
        // The apex of the first bend: banked, and negative (left low).
        let apex = bank_at(&track, 360.0, 0.0);
        assert!((apex + 0.3).abs() < 1e-3, "apex banking {apex}");
    }

    #[test]
    fn banking_that_starts_after_the_bend_is_moved_onto_it() {
        // Laid on the straight after the second bend (which ends at the
        // start line), the way the lap-fraction windows drifted.
        let mut track = stadium(|s| if (5.0..150.0).contains(&s) { 0.3 } else { 0.0 });
        realign_banking(&mut track).unwrap();
        // Mid-straight: nothing left.
        assert_eq!(bank_at(&track, 150.0, -60.0), 0.0);
        // The second bend's apex carries it.
        assert!((bank_at(&track, -60.0, 0.0) + 0.3).abs() < 1e-3);
        // The first bend was never banked and still is not.
        assert_eq!(bank_at(&track, 360.0, 0.0), 0.0);
    }

    #[test]
    fn realigning_twice_changes_nothing() {
        let mut track = stadium(|s| {
            if (280.0..520.0).contains(&s) {
                0.2
            } else {
                0.0
            }
        });
        realign_banking(&mut track).unwrap();
        let once = track.clone();
        let report = realign_banking(&mut track).unwrap();
        assert!(!report.changed());
        assert_eq!(once, track);
    }

    #[test]
    fn a_right_hander_is_banked_with_its_left_edge_high() {
        // The same stadium run the other way round: every bend a right.
        let mut track = stadium(|s| {
            if (300.0..490.0).contains(&s) {
                0.3
            } else {
                0.0
            }
        });
        track.nodes.reverse();
        realign_banking(&mut track).unwrap();
        assert!(bank_at(&track, 360.0, 0.0) > 0.29);
    }
}
