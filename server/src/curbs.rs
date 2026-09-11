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
        if self.version != 1 {
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
        if band.is_empty() {
            return 0.0;
        }
        let sample = (station_m / self.step_m).round();
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
            version: 1,
            step_m: 1.0,
            //         0    1    2    3
            left_cm: vec![0, 150, 150, 0],
            right_cm: vec![100, 0, 0, 0],
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
        b.version = 2;
        assert!(b.validate().is_err());
        let mut b = bands();
        b.step_m = 0.0;
        assert!(b.validate().is_err());
    }

    #[test]
    fn sidecar_sits_next_to_the_track_file() {
        let path = CurbBands::sidecar_path(Path::new("content/tracks/real/Monza.yaml"));
        assert!(path.ends_with("Monza.curbs.msgpack"));
    }
}
