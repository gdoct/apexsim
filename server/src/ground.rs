//! Baked ground heightfield: the height of whatever the client renders at a
//! world position (road surface, verge, runoff, terrain).
//!
//! The track editor's `ats-export` writes `<Track>.ground.msgpack` next to the
//! track YAML while it bakes the Unreal scene, sampling the same ground model
//! the client draws. The sim reads it so a car that leaves the asphalt sits on
//! the grass the player sees rather than staying at road height (or dropping
//! onto a neighbouring section's elevation when the nearest-centerline search
//! hands over). It is optional: without the sidecar the centerline elevation
//! is used everywhere, as before.

use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

/// Row-major grid of ground heights in centimetres, in the server frame
/// (metres, +X forward at the start line, +Y left, +Z up).
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct GroundHeightfield {
    pub version: u32,
    pub origin_x: f32,
    pub origin_y: f32,
    pub cell_m: f32,
    pub cols: u32,
    pub rows: u32,
    pub heights_cm: Vec<i16>,
}

#[derive(Debug, thiserror::Error)]
pub enum GroundLoadError {
    #[error("io error: {0}")]
    Io(#[from] std::io::Error),
    #[error("decode error: {0}")]
    Decode(#[from] rmp_serde::decode::Error),
    #[error("invalid heightfield: {0}")]
    Invalid(String),
}

impl GroundHeightfield {
    /// Sidecar path for a track file: `Monza.yaml` -> `Monza.ground.msgpack`.
    pub fn sidecar_path(track_file: &Path) -> PathBuf {
        let stem = track_file
            .file_stem()
            .and_then(|s| s.to_str())
            .unwrap_or("track");
        track_file.with_file_name(format!("{}.ground.msgpack", stem))
    }

    pub fn load(path: &Path) -> Result<Self, GroundLoadError> {
        let bytes = std::fs::read(path)?;
        let field: Self = rmp_serde::from_slice(&bytes)?;
        field.validate()?;
        Ok(field)
    }

    pub fn validate(&self) -> Result<(), GroundLoadError> {
        if self.version != 1 {
            return Err(GroundLoadError::Invalid(format!(
                "unsupported version {}",
                self.version
            )));
        }
        if self.cols < 2 || self.rows < 2 {
            return Err(GroundLoadError::Invalid("grid smaller than 2x2".into()));
        }
        if !(self.cell_m.is_finite() && self.cell_m > 0.0) {
            return Err(GroundLoadError::Invalid(
                "cell size must be positive".into(),
            ));
        }
        let expected = self.cols as usize * self.rows as usize;
        if self.heights_cm.len() != expected {
            return Err(GroundLoadError::Invalid(format!(
                "{} samples for a {}x{} grid",
                self.heights_cm.len(),
                self.cols,
                self.rows
            )));
        }
        Ok(())
    }

    #[inline]
    fn height_at_cell(&self, col: usize, row: usize) -> f32 {
        self.heights_cm[row * self.cols as usize + col] as f32 * 0.01
    }

    /// Bilinearly interpolated ground height in metres. Positions outside the
    /// grid clamp to its edge so the surface is continuous everywhere; the
    /// exporter pads the grid well past the track so this only matters for a
    /// car that has left the world entirely.
    pub fn sample(&self, x: f32, y: f32) -> f32 {
        let max_col = (self.cols - 1) as f32;
        let max_row = (self.rows - 1) as f32;
        let fx = ((x - self.origin_x) / self.cell_m).clamp(0.0, max_col);
        let fy = ((y - self.origin_y) / self.cell_m).clamp(0.0, max_row);
        let c0 = fx.floor().min(max_col - 1.0) as usize;
        let r0 = fy.floor().min(max_row - 1.0) as usize;
        let tx = fx - c0 as f32;
        let ty = fy - r0 as f32;
        let h00 = self.height_at_cell(c0, r0);
        let h10 = self.height_at_cell(c0 + 1, r0);
        let h01 = self.height_at_cell(c0, r0 + 1);
        let h11 = self.height_at_cell(c0 + 1, r0 + 1);
        let top = h00 + (h10 - h00) * tx;
        let bottom = h01 + (h11 - h01) * tx;
        top + (bottom - top) * ty
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn field() -> GroundHeightfield {
        GroundHeightfield {
            version: 1,
            origin_x: -10.0,
            origin_y: -10.0,
            cell_m: 10.0,
            cols: 3,
            rows: 2,
            // row 0: 0 m, 1 m, 2 m ; row 1: 10 m, 11 m, 12 m
            heights_cm: vec![0, 100, 200, 1000, 1100, 1200],
        }
    }

    #[test]
    fn samples_bilinearly() {
        let f = field();
        assert!((f.sample(-10.0, -10.0) - 0.0).abs() < 1e-5);
        assert!((f.sample(0.0, -10.0) - 1.0).abs() < 1e-5);
        assert!((f.sample(-5.0, -10.0) - 0.5).abs() < 1e-5);
        assert!((f.sample(-5.0, -5.0) - 5.5).abs() < 1e-5);
        assert!((f.sample(10.0, 0.0) - 12.0).abs() < 1e-5);
    }

    #[test]
    fn clamps_outside_the_grid() {
        let f = field();
        assert!((f.sample(-100.0, -100.0) - 0.0).abs() < 1e-5);
        assert!((f.sample(100.0, 100.0) - 12.0).abs() < 1e-5);
    }

    #[test]
    fn rejects_mismatched_sizes() {
        let mut f = field();
        f.heights_cm.pop();
        assert!(f.validate().is_err());
    }

    #[test]
    fn sidecar_path_uses_the_track_stem() {
        let p = GroundHeightfield::sidecar_path(Path::new("content/tracks/real/Monza.yaml"));
        assert!(p.ends_with("Monza.ground.msgpack"));
    }

    #[test]
    fn msgpack_roundtrip_named() {
        let f = field();
        let bytes = rmp_serde::to_vec_named(&f).unwrap();
        let back: GroundHeightfield = rmp_serde::from_slice(&bytes).unwrap();
        assert_eq!(f, back);
    }
}
