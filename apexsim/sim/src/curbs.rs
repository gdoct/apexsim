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

use std::path::Path;

/// The sidecar's type, from the formats crate
/// (`apexsim-lib/formats/src/curbs.rs`): curb width outward from each road
/// edge, sampled every `step_m` metres of centerline station, with the
/// tarmac run-off and the gravel and sand traps beside it. What the sim
/// asks of it is [`CurbQuery`].
pub use apexsim_formats::curbs::{CurbBands, CURB_BANDS_VERSION};

#[derive(Debug, thiserror::Error)]
pub enum CurbLoadError {
    #[error("io error: {0}")]
    Io(#[from] std::io::Error),
    #[error("decode error: {0}")]
    Decode(#[from] rmp_serde::decode::Error),
    #[error("invalid curb bands: {0}")]
    Invalid(String),
}

/// The server's reading of [`CurbBands`]: loading and checking the
/// sidecar, and what is past the road edge at a station. The sidecar's
/// path is the crate's `CurbBands::sidecar_path`.
pub trait CurbQuery: Sized {
    fn load(path: &Path) -> Result<Self, CurbLoadError>;
    /// The sidecar's bytes, decoded and checked as [`Self::load`] does.
    fn from_bytes(bytes: &[u8]) -> Result<Self, CurbLoadError>;
    fn validate(&self) -> Result<(), CurbLoadError>;
    /// The trap ground at `overhang_m` past the road edge at `station_m`,
    /// on the side `lateral_offset` points to: gravel where a gravel trap
    /// is (laid over a sand one, as the bake draws it), sand where a sand
    /// trap is, `None` elsewhere.
    fn trap_at(
        &self,
        station_m: f32,
        lateral_offset: f32,
        overhang_m: f32,
    ) -> Option<crate::physics::OffGround>;
    /// The ground past the bands ([`CurbBands::ground`]).
    fn base_ground(&self) -> crate::physics::OffGround;
    /// Number of station samples.
    fn len(&self) -> usize;
    fn is_empty(&self) -> bool;
    /// Curb width in metres reaching outward from the road edge at
    /// `station_m`, on the side `lateral_offset` points to (positive = right
    /// of the centerline, the sim's convention). 0 where there is no curb.
    ///
    /// The station wraps, so a closed lap reads the same band whichever lap
    /// the car is on, and a station past the end of an open track folds back
    /// rather than panicking.
    fn width_at(&self, station_m: f32, lateral_offset: f32) -> f32;
    /// How far the tarmac run-off reaches past the road edge at
    /// `station_m` on the side `lateral_offset` points to, metres; 0 where
    /// the run-off is grass or gravel (or the sidecar predates the band).
    fn runoff_at(&self, station_m: f32, lateral_offset: f32) -> f32;
}

impl CurbQuery for CurbBands {
    fn load(path: &Path) -> Result<Self, CurbLoadError> {
        <Self as CurbQuery>::from_bytes(&std::fs::read(path)?)
    }

    fn from_bytes(bytes: &[u8]) -> Result<Self, CurbLoadError> {
        let bands: Self = rmp_serde::from_slice(bytes)?;
        bands.validate()?;
        Ok(bands)
    }

    fn validate(&self) -> Result<(), CurbLoadError> {
        if !(1..=CURB_BANDS_VERSION).contains(&self.version) {
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
        for (name, band) in [
            ("gravel left", &self.gravel_left_cm),
            ("gravel right", &self.gravel_right_cm),
            ("sand left", &self.sand_left_cm),
            ("sand right", &self.sand_right_cm),
        ] {
            if !band.is_empty() && band.len() != self.left_cm.len() {
                return Err(CurbLoadError::Invalid(format!(
                    "{} {name} samples against {} curb samples",
                    band.len(),
                    self.left_cm.len()
                )));
            }
        }
        Ok(())
    }

    fn trap_at(
        &self,
        station_m: f32,
        lateral_offset: f32,
        overhang_m: f32,
    ) -> Option<crate::physics::OffGround> {
        use crate::physics::OffGround;
        let right = lateral_offset >= 0.0;
        let (gravel, sand) = if right {
            (&self.gravel_right_cm, &self.sand_right_cm)
        } else {
            (&self.gravel_left_cm, &self.sand_left_cm)
        };
        let within = |band: &[[u16; 2]]| {
            if band.is_empty() {
                return false;
            }
            let sample = (station_m / self.step_m).round();
            if !sample.is_finite() {
                return false;
            }
            let [inner, outer] = band[(sample as i64).rem_euclid(band.len() as i64) as usize];
            outer > inner && overhang_m >= inner as f32 * 0.01 && overhang_m <= outer as f32 * 0.01
        };
        if within(gravel) {
            Some(OffGround::Gravel)
        } else if within(sand) {
            Some(OffGround::Sand)
        } else {
            None
        }
    }

    fn base_ground(&self) -> crate::physics::OffGround {
        crate::physics::OffGround::from_kind(&self.ground)
    }

    fn len(&self) -> usize {
        self.left_cm.len()
    }

    fn is_empty(&self) -> bool {
        self.left_cm.is_empty()
    }

    fn width_at(&self, station_m: f32, lateral_offset: f32) -> f32 {
        let band = if lateral_offset >= 0.0 {
            &self.right_cm
        } else {
            &self.left_cm
        };
        read_band(band, self.step_m, station_m)
    }

    fn runoff_at(&self, station_m: f32, lateral_offset: f32) -> f32 {
        let band = if lateral_offset >= 0.0 {
            &self.runoff_right_cm
        } else {
            &self.runoff_left_cm
        };
        read_band(band, self.step_m, station_m)
    }
}

fn read_band(band: &[u16], step_m: f32, station_m: f32) -> f32 {
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
            gravel_left_cm: Vec::new(),
            gravel_right_cm: vec![[0, 0], [200, 900], [200, 900], [0, 0]],
            sand_left_cm: vec![[0, 0], [100, 500], [0, 0], [0, 0]],
            sand_right_cm: Vec::new(),
            ground: String::new(),
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
        b.version = 4;
        assert!(b.validate().is_err());
        let mut b = bands();
        b.gravel_right_cm.pop();
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
    fn reads_the_traps_between_their_edges() {
        use crate::physics::OffGround;
        let b = bands();
        assert_eq!(b.trap_at(1.0, 5.0, 1.0), None, "short of the gravel");
        assert_eq!(b.trap_at(1.0, 5.0, 4.0), Some(OffGround::Gravel));
        assert_eq!(b.trap_at(2.0, 5.0, 9.0), Some(OffGround::Gravel));
        assert_eq!(b.trap_at(2.0, 5.0, 9.5), None, "past the gravel");
        assert_eq!(b.trap_at(1.0, -5.0, 4.0), Some(OffGround::Sand));
        assert_eq!(b.trap_at(2.0, -5.0, 4.0), None);
        assert_eq!(b.trap_at(0.0, 5.0, 4.0), None);
        assert_eq!(b.trap_at(f32::NAN, 5.0, 4.0), None);
        assert_eq!(b.base_ground(), OffGround::Grass);
    }

    #[test]
    fn reads_the_runoff_reach_for_the_side_the_car_is_on() {
        let b = bands();
        assert_eq!(b.runoff_at(1.0, -3.0), 8.0, "tarmac 8 m out on the left");
        assert_eq!(b.runoff_at(1.0, 3.0), 0.0, "none on the right");
    }

    #[test]
    fn sidecar_sits_next_to_the_track_file() {
        let path = CurbBands::sidecar_path(Path::new(
            "../apexsim-content/tracks/default/Monzarella/Monzarella.yaml",
        ));
        assert!(path.ends_with("Monzarella.curbs.msgpack"));
    }
}
