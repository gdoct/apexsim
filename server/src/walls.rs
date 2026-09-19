//! Baked track walls: what stops a car that leaves the road.
//!
//! The barriers, tire walls, pit walls, grandstands and buildings are
//! authored in the track editor's `.ats` scene and reach the client as
//! meshes; the sim needs only where their solid faces are. `ats-export`
//! writes them to `<Track>.walls.msgpack` next to the track YAML while it
//! bakes the Unreal scene: one line segment per barrier face in the ground
//! plane, with the ground height at its base and how tall it is, so the
//! car is stopped by the same armco the player is looking at. Deck parapets
//! and the abutment walls of an underpass are in it too, tall enough to be
//! told apart by height from the road they are not on.
//!
//! Without the sidecar nothing stops a car off the road, as before.
//!
//! The segments are bucketed into a uniform grid at load, so the physics
//! tick asks only for the few near a car.

use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

/// What a wall is made of, which sets how a car bounces off it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WallKind {
    /// Steel armco or a fence: springy.
    Armco = 0,
    /// Tires or TecPro: absorbs the hit.
    Tires = 1,
    /// Concrete, a building, a stand, a bridge parapet: hard.
    Concrete = 2,
    /// A sausage kerb: the car rides over it and takes a jolt, it is
    /// never pushed out (see `physics::resolve_wall_contacts`).
    Kerb = 3,
}

impl WallKind {
    pub fn from_u8(kind: u8) -> Option<Self> {
        match kind {
            0 => Some(WallKind::Armco),
            1 => Some(WallKind::Tires),
            2 => Some(WallKind::Concrete),
            3 => Some(WallKind::Kerb),
            _ => None,
        }
    }

    /// Bounce-back as a fraction of the closing speed.
    pub fn restitution(self) -> f32 {
        match self {
            WallKind::Armco => 0.25,
            WallKind::Tires => 0.08,
            WallKind::Concrete => 0.3,
            WallKind::Kerb => 0.0,
        }
    }

    /// Coulomb friction of the car's flank scraping along the wall.
    pub fn friction(self) -> f32 {
        match self {
            WallKind::Armco => 0.5,
            WallKind::Tires => 0.8,
            WallKind::Concrete => 0.6,
            WallKind::Kerb => 0.9,
        }
    }
}

/// One solid face in the ground plane, track frame (metres, +Y left).
#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq)]
pub struct WallSegment {
    pub x0: f32,
    pub y0: f32,
    pub x1: f32,
    pub y1: f32,
    /// Ground height at the base of the wall.
    pub z: f32,
    /// How far the wall reaches above its base.
    pub height_m: f32,
    /// [`WallKind`] as written by the exporter.
    pub kind: u8,
}

impl WallSegment {
    pub fn kind(&self) -> WallKind {
        WallKind::from_u8(self.kind).unwrap_or(WallKind::Concrete)
    }

    pub fn length(&self) -> f32 {
        (self.x1 - self.x0).hypot(self.y1 - self.y0)
    }

    fn is_finite(&self) -> bool {
        [self.x0, self.y0, self.x1, self.y1, self.z, self.height_m]
            .iter()
            .all(|v| v.is_finite())
    }
}

/// The sidecar as written (`rmp_serde::to_vec_named`).
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct WallFile {
    pub version: u32,
    pub segments: Vec<WallSegment>,
}

#[derive(Debug, thiserror::Error)]
pub enum WallLoadError {
    #[error("io error: {0}")]
    Io(#[from] std::io::Error),
    #[error("decode error: {0}")]
    Decode(#[from] rmp_serde::decode::Error),
    #[error("invalid walls: {0}")]
    Invalid(String),
}

/// Grid cell size, metres. A car's query circle is under 4 m across, so a
/// query touches one to four cells.
const CELL_M: f32 = 16.0;
/// A segment thinner than this is a point and is dropped.
const MIN_LENGTH_M: f32 = 0.01;

/// The walls of a track with a grid over them.
#[derive(Debug, Clone)]
pub struct Walls {
    segments: Vec<WallSegment>,
    origin: (f32, f32),
    cols: usize,
    rows: usize,
    /// CSR: cell `c` owns `entries[starts[c]..starts[c + 1]]`.
    starts: Vec<u32>,
    entries: Vec<u32>,
}

impl Walls {
    /// Sidecar path for a track file: `Monza.yaml` -> `Monza.walls.msgpack`.
    pub fn sidecar_path(track_file: &Path) -> PathBuf {
        let stem = track_file
            .file_stem()
            .and_then(|s| s.to_str())
            .unwrap_or("track");
        track_file.with_file_name(format!("{}.walls.msgpack", stem))
    }

    pub fn load(path: &Path) -> Result<Self, WallLoadError> {
        let bytes = std::fs::read(path)?;
        let file: WallFile = rmp_serde::from_slice(&bytes)?;
        Self::from_file(file)
    }

    pub fn from_file(file: WallFile) -> Result<Self, WallLoadError> {
        if file.version != 1 {
            return Err(WallLoadError::Invalid(format!(
                "unsupported version {}",
                file.version
            )));
        }
        if let Some(bad) = file.segments.iter().find(|s| !s.is_finite()) {
            return Err(WallLoadError::Invalid(format!(
                "non-finite segment {:?}",
                bad
            )));
        }
        Ok(Self::from_segments(file.segments))
    }

    /// Build the index over `segments`; degenerate ones are dropped.
    pub fn from_segments(segments: Vec<WallSegment>) -> Self {
        let segments: Vec<WallSegment> = segments
            .into_iter()
            .filter(|s| s.is_finite() && s.length() >= MIN_LENGTH_M && s.height_m > 0.0)
            .collect();

        let (mut min_x, mut min_y, mut max_x, mut max_y) = (
            f32::INFINITY,
            f32::INFINITY,
            f32::NEG_INFINITY,
            f32::NEG_INFINITY,
        );
        for s in &segments {
            min_x = min_x.min(s.x0).min(s.x1);
            min_y = min_y.min(s.y0).min(s.y1);
            max_x = max_x.max(s.x0).max(s.x1);
            max_y = max_y.max(s.y0).max(s.y1);
        }
        if segments.is_empty() {
            return Self {
                segments,
                origin: (0.0, 0.0),
                cols: 0,
                rows: 0,
                starts: vec![0],
                entries: Vec::new(),
            };
        }
        let origin = (min_x - 1.0, min_y - 1.0);
        let cols = ((max_x + 1.0 - origin.0) / CELL_M).ceil().max(1.0) as usize;
        let rows = ((max_y + 1.0 - origin.1) / CELL_M).ceil().max(1.0) as usize;

        // Two passes: count per cell, then fill — a segment lands in every
        // cell its (slightly padded) bounding box touches.
        let cell_range = |s: &WallSegment| {
            let pad = 0.5;
            let c0 = ((s.x0.min(s.x1) - pad - origin.0) / CELL_M)
                .floor()
                .max(0.0) as usize;
            let c1 = ((s.x0.max(s.x1) + pad - origin.0) / CELL_M).floor() as usize;
            let r0 = ((s.y0.min(s.y1) - pad - origin.1) / CELL_M)
                .floor()
                .max(0.0) as usize;
            let r1 = ((s.y0.max(s.y1) + pad - origin.1) / CELL_M).floor() as usize;
            (c0, c1.min(cols - 1), r0, r1.min(rows - 1))
        };
        let mut counts = vec![0u32; cols * rows];
        for s in &segments {
            let (c0, c1, r0, r1) = cell_range(s);
            for r in r0..=r1 {
                for c in c0..=c1 {
                    counts[r * cols + c] += 1;
                }
            }
        }
        let mut starts = Vec::with_capacity(cols * rows + 1);
        let mut acc = 0u32;
        for n in &counts {
            starts.push(acc);
            acc += n;
        }
        starts.push(acc);
        let mut fill = starts.clone();
        let mut entries = vec![0u32; acc as usize];
        for (idx, s) in segments.iter().enumerate() {
            let (c0, c1, r0, r1) = cell_range(s);
            for r in r0..=r1 {
                for c in c0..=c1 {
                    let cell = r * cols + c;
                    entries[fill[cell] as usize] = idx as u32;
                    fill[cell] += 1;
                }
            }
        }

        Self {
            segments,
            origin,
            cols,
            rows,
            starts,
            entries,
        }
    }

    pub fn segments(&self) -> &[WallSegment] {
        &self.segments
    }

    pub fn len(&self) -> usize {
        self.segments.len()
    }

    pub fn is_empty(&self) -> bool {
        self.segments.is_empty()
    }

    /// Indices of every segment whose cell touches the circle at `(x, y)`
    /// of `radius`, ascending and without repeats, into `out` (cleared
    /// first). Ascending order is what keeps the solver's contact order,
    /// and so the sim, deterministic.
    pub fn candidates(&self, x: f32, y: f32, radius: f32, out: &mut Vec<u32>) {
        out.clear();
        if self.segments.is_empty() || !(x.is_finite() && y.is_finite() && radius.is_finite()) {
            return;
        }
        let c0 = (x - radius - self.origin.0) / CELL_M;
        let c1 = (x + radius - self.origin.0) / CELL_M;
        let r0 = (y - radius - self.origin.1) / CELL_M;
        let r1 = (y + radius - self.origin.1) / CELL_M;
        if c1 < 0.0 || r1 < 0.0 || c0 >= self.cols as f32 || r0 >= self.rows as f32 {
            return;
        }
        let c0 = c0.floor().max(0.0) as usize;
        let c1 = (c1.floor().max(0.0) as usize).min(self.cols - 1);
        let r0 = r0.floor().max(0.0) as usize;
        let r1 = (r1.floor().max(0.0) as usize).min(self.rows - 1);
        for r in r0..=r1 {
            for c in c0..=c1 {
                let cell = r * self.cols + c;
                out.extend_from_slice(
                    &self.entries[self.starts[cell] as usize..self.starts[cell + 1] as usize],
                );
            }
        }
        out.sort_unstable();
        out.dedup();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn seg(x0: f32, y0: f32, x1: f32, y1: f32) -> WallSegment {
        WallSegment {
            x0,
            y0,
            x1,
            y1,
            z: 0.0,
            height_m: 1.0,
            kind: 0,
        }
    }

    #[test]
    fn sidecar_sits_next_to_the_track_file() {
        let path = Walls::sidecar_path(Path::new("content/tracks/real/Monza.yaml"));
        assert!(path.ends_with("Monza.walls.msgpack"));
    }

    #[test]
    fn candidates_are_the_nearby_segments_in_order() {
        let walls = Walls::from_segments(vec![
            seg(100.0, 0.0, 104.0, 0.0),
            seg(0.0, 0.0, 4.0, 0.0),
            seg(4.0, 0.0, 8.0, 0.0),
            seg(-50.0, -50.0, -50.0, -46.0),
        ]);
        let mut out = Vec::new();
        walls.candidates(4.0, 1.0, 2.0, &mut out);
        assert_eq!(out, vec![1, 2]);
        walls.candidates(500.0, 500.0, 2.0, &mut out);
        assert!(out.is_empty(), "far outside the grid: {out:?}");
        walls.candidates(-50.0, -48.0, 2.0, &mut out);
        assert_eq!(out, vec![3]);
    }

    #[test]
    fn a_long_segment_is_found_from_its_middle() {
        let walls = Walls::from_segments(vec![seg(0.0, 0.0, 200.0, 0.0)]);
        let mut out = Vec::new();
        walls.candidates(100.0, 0.5, 2.0, &mut out);
        assert_eq!(out, vec![0]);
    }

    #[test]
    fn degenerate_segments_are_dropped_and_bad_files_refused() {
        let walls = Walls::from_segments(vec![seg(1.0, 1.0, 1.0, 1.0), seg(0.0, 0.0, 1.0, 0.0)]);
        assert_eq!(walls.len(), 1);
        assert!(Walls::from_file(WallFile {
            version: 2,
            segments: Vec::new()
        })
        .is_err());
        assert!(Walls::from_file(WallFile {
            version: 1,
            segments: vec![seg(f32::NAN, 0.0, 1.0, 0.0)]
        })
        .is_err());
        let empty = Walls::from_file(WallFile {
            version: 1,
            segments: Vec::new(),
        })
        .unwrap();
        let mut out = Vec::new();
        empty.candidates(0.0, 0.0, 5.0, &mut out);
        assert!(out.is_empty());
    }

    #[test]
    fn roundtrips_through_msgpack() {
        let file = WallFile {
            version: 1,
            segments: vec![seg(0.0, 0.0, 4.0, 0.0)],
        };
        let bytes = rmp_serde::to_vec_named(&file).unwrap();
        let back: WallFile = rmp_serde::from_slice(&bytes).unwrap();
        assert_eq!(back, file);
    }
}
