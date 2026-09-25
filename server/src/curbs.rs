//! Baked curb bands: how far the curbs reach past each road edge.
//!
//! The curbs are authored in the track editor's `.ats` scene and baked into
//! meshes for the client, but the sim needs only one number from them — the
//! width of the strip beyond the asphalt that is still track. The editor's
//! `ats-export` writes it to `<Track>.curbs.msgpack` next to the track YAML
//! while it bakes the Unreal scene, so a car with two wheels on a curb is
//! judged on the same geometry the player is looking at.
//!
//! Without the sidecar the road edge is the track limit, as before: the
//! curbs then read as grass and a driver who uses them is slowed for it.

use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

/// Curb width outward from each road edge, sampled every [`CurbBands::step_m`]
/// meters of centerline station. Sample `i` is the cross-section at station
/// `i * step_m`; the value is in centimetres, 0 where that edge has no curb.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct CurbBands {
    pub version: u32,
    pub step_m: f32,
    pub left_cm: Vec<u16>,
    pub right_cm: Vec<u16>,
    /// Version 2: how far the prepared tarmac run-off reaches past each
    /// road edge, centimetres, 0 where there is none. Empty in a version 1
    /// file, which reads as no tarmac anywhere.
    #[serde(default)]
    pub runoff_left_cm: Vec<u16>,
    #[serde(default)]
    pub runoff_right_cm: Vec<u16>,
}

#[derive(Debug, thiserror::Error)]
pub enum CurbLoadError {
    #[error("io error: {0}")]
    Io(#[from] std::io::Error),
    #[error("decode error: {0}")]
    Decode(#[from] rmp_serde::decode::Error),
    #[error("invalid curb bands: {0}")]
    Invalid(String),
}

impl CurbBands {
    /// Sidecar path for a track file: `Monza.yaml` -> `Monza.curbs.msgpack`.
    pub fn sidecar_path(track_file: &Path) -> PathBuf {
        let stem = track_file
            .file_stem()
            .and_then(|s| s.to_str())
            .unwrap_or("track");
        track_file.with_file_name(format!("{}.curbs.msgpack", stem))
    }

    pub fn load(path: &Path) -> Result<Self, CurbLoadError> {
        let bytes = std::fs::read(path)?;
        let bands: Self = rmp_serde::from_slice(&bytes)?;
        bands.validate()?;
        Ok(bands)
    }

    pub fn validate(&self) -> Result<(), CurbLoadError> {
        if !(1..=2).contains(&self.version) {
            return Err(CurbLoadError::Invalid(format!(
                "unsupported version {}",
                self.version
            )));
        }
        if !(self.step_m.is_finite() && self.step_m > 0.0) {
            return Err(CurbLoadError::Invalid(
                "station step must be positive".into(),
            ));
        }
        if self.left_cm.is_empty() {
            return Err(CurbLoadError::Invalid("no samples".into()));
        }
        if self.left_cm.len() != self.right_cm.len() {
            return Err(CurbLoadError::Invalid(format!(
                "{} left samples against {} right",
                self.left_cm.len(),
                self.right_cm.len()
            )));
        }
        let runoff_ok = (self.runoff_left_cm.is_empty() && self.runoff_right_cm.is_empty())
            || (self.runoff_left_cm.len() == self.left_cm.len()
                && self.runoff_right_cm.len() == self.right_cm.len());
        if !runoff_ok {
            return Err(CurbLoadError::Invalid(format!(
                "{} / {} run-off samples against {} curb samples",
                self.runoff_left_cm.len(),
                self.runoff_right_cm.len(),
                self.left_cm.len()
            )));
        }
        Ok(())
    }

    /// Number of station samples.
    pub fn len(&self) -> usize {
        self.left_cm.len()
    }

    pub fn is_empty(&self) -> bool {
        self.left_cm.is_empty()
    }

    /// Curb width in metres reaching outward from the road edge at
    /// `station_m`, on the side `lateral_offset` points to (positive = right
    /// of the centerline, the sim's convention). 0 where there is no curb.
    ///
    /// The station wraps, so a closed lap reads the same band whichever lap
    /// the car is on, and a station past the end of an open track folds back
    /// rather than panicking.
    pub fn width_at(&self, station_m: f32, lateral_offset: f32) -> f32 {
        let band = if lateral_offset >= 0.0 {
            &self.right_cm
        } else {
            &self.left_cm
        };
        Self::read(band, self.step_m, station_m)
    }

    /// How far the tarmac run-off reaches past the road edge at
    /// `station_m` on the side `lateral_offset` points to, metres; 0 where
    /// the run-off is grass or gravel (or the sidecar predates the band).
    pub fn runoff_at(&self, station_m: f32, lateral_offset: f32) -> f32 {
        let band = if lateral_offset >= 0.0 {
            &self.runoff_right_cm
        } else {
            &self.runoff_left_cm
        };
        Self::read(band, self.step_m, station_m)
    }

    fn read(band: &[u16], step_m: f32, station_m: f32) -> f32 {
        if band.is_empty() {
            return 0.0;
        }
        let sample = (station_m / step_m).round();
        if !sample.is_finite() {
            return 0.0;
        }
        let idx = (sample as i64).rem_euclid(band.len() as i64) as usize;
        band[idx] as f32 * 0.01
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn bands() -> CurbBands {
        CurbBands {
            version: 2,
            step_m: 1.0,
            //         0    1    2    3
            left_cm: vec![0, 150, 150, 0],
            right_cm: vec![100, 0, 0, 0],
            runoff_left_cm: vec![0, 800, 800, 0],
            runoff_right_cm: vec![0, 0, 0, 0],
        }
    }

    #[test]
    fn reads_the_band_for_the_side_the_car_is_on() {
        let b = bands();
        assert_eq!(b.width_at(1.0, -3.0), 1.5, "left of the centerline");
        assert_eq!(b.width_at(1.0, 3.0), 0.0, "right of the centerline");
        assert_eq!(b.width_at(0.0, 3.0), 1.0);
    }

    #[test]
    fn stations_round_to_the_nearest_sample_and_wrap() {
        let b = bands();
        assert_eq!(b.width_at(1.4, -1.0), 1.5);
        assert_eq!(b.width_at(0.6, -1.0), 1.5);
        assert_eq!(b.width_at(0.4, -1.0), 0.0);
        // A second lap of a 4 m "track" reads the same bands.
        assert_eq!(b.width_at(5.0, -1.0), 1.5);
        assert_eq!(b.width_at(-3.0, -1.0), 1.5);
    }

    #[test]
    fn nonsense_stations_are_not_curbs() {
        let b = bands();
        assert_eq!(b.width_at(f32::NAN, -1.0), 0.0);
        assert_eq!(b.width_at(f32::INFINITY, -1.0), 0.0);
    }

    #[test]
    fn validate_rejects_mismatched_bands() {
        let mut b = bands();
        b.right_cm.pop();
        assert!(b.validate().is_err());
        let mut b = bands();
        b.version = 3;
        assert!(b.validate().is_err());
        let mut b = bands();
        b.step_m = 0.0;
        assert!(b.validate().is_err());
        // Version 2's run-off bands: both empty (a version 1 file) or both
        // the curb bands' length.
        let mut b = bands();
        b.runoff_left_cm.pop();
        assert!(b.validate().is_err());
        let mut b = bands();
        b.runoff_left_cm.clear();
        b.runoff_right_cm.clear();
        assert!(b.validate().is_ok());
        assert_eq!(b.runoff_at(1.0, -3.0), 0.0, "no run-off band reads as none");
    }

    #[test]
    fn reads_the_runoff_reach_for_the_side_the_car_is_on() {
        let b = bands();
        assert_eq!(b.runoff_at(1.0, -3.0), 8.0, "tarmac 8 m out on the left");
        assert_eq!(b.runoff_at(1.0, 3.0), 0.0, "none on the right");
    }

    #[test]
    fn sidecar_sits_next_to_the_track_file() {
        let path = CurbBands::sidecar_path(Path::new("content/tracks/real/Monza.yaml"));
        assert!(path.ends_with("Monza.curbs.msgpack"));
    }
}
