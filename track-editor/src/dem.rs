//! The real shape of the land around a circuit.
//!
//! Every circuit's ground used to be an inverse-distance average of the
//! road's own elevations, which is a smooth blanket that decays to the
//! mean track height and has no independent existence: no hills, no
//! ridges, no valley, nothing at all past the 800 m the mesh reached.
//! The Red Bull Ring sits in the Murtal with wooded slopes rising three
//! or four hundred metres within a kilometre and the Seetaler Alpen
//! behind them, and none of it was there.
//!
//! `scripts/dem_fetch.py` writes a sidecar beside each track YAML from
//! the Copernicus GLO-30 elevation model, georeferenced onto the track's
//! own frame by the same fit the layout dossier uses. This reads it.
//!
//! Two grids, because the two jobs are different. `inner` is 10 m posts
//! over the circuit and a kilometre around it, which is the ground a car
//! can reach and the ground the camera looks across. `outer` is 90 m
//! posts out to eight kilometres, which is the skyline: too coarse to
//! drive on and far too much to draw at 10 m, but it is the difference
//! between a horizon and a green pancake fading into fog.
//!
//! The sidecar is generated and gitignored like the others, so a track
//! without one simply falls back to the old blanket. Nothing here is
//! required for a circuit to load.

use std::path::{Path, PathBuf};

use serde::Deserialize;

/// The sidecar's own format version, bumped when the layout changes.
pub const DEM_VERSION: u32 = 1;

#[derive(Debug, Clone, Deserialize)]
pub struct DemFile {
    #[serde(default)]
    pub version: u32,
    /// Who the elevations belong to; carried so a build can credit them.
    #[serde(default)]
    pub source: String,
    /// The near grid: 10 m posts over the circuit and about a kilometre
    /// beyond it.
    pub inner: DemGrid,
    /// The far grid: 90 m posts out to the skyline.
    pub outer: DemGrid,
}

#[derive(Debug, Clone, Deserialize)]
pub struct DemGrid {
    pub cell_m: f32,
    pub cols: usize,
    pub rows: usize,
    pub origin_x: f32,
    pub origin_y: f32,
    /// Row-major, `heights[row * cols + col]` at
    /// `(origin_x + col * cell_m, origin_y + row * cell_m)`, in the
    /// server frame with the same datum as the track's own z.
    pub heights: Vec<f32>,
}

impl DemGrid {
    /// Bilinear sample. Queries outside the grid clamp to its border,
    /// which is what keeps the far field flat rather than falling away.
    pub fn height_at(&self, x: f32, y: f32) -> f32 {
        if self.cols == 0 || self.rows == 0 || self.heights.is_empty() {
            return 0.0;
        }
        let fx = ((x - self.origin_x) / self.cell_m).clamp(0.0, (self.cols - 1) as f32);
        let fy = ((y - self.origin_y) / self.cell_m).clamp(0.0, (self.rows - 1) as f32);
        let (c0, r0) = (fx.floor() as usize, fy.floor() as usize);
        let c1 = (c0 + 1).min(self.cols - 1);
        let r1 = (r0 + 1).min(self.rows - 1);
        let (tx, ty) = (fx - c0 as f32, fy - r0 as f32);
        let at = |r: usize, c: usize| self.heights[r * self.cols + c];
        let top = at(r0, c0) * (1.0 - tx) + at(r0, c1) * tx;
        let bottom = at(r1, c0) * (1.0 - tx) + at(r1, c1) * tx;
        top * (1.0 - ty) + bottom * ty
    }

    /// `(min_x, min_y, max_x, max_y)` the grid covers.
    pub fn bounds(&self) -> (f32, f32, f32, f32) {
        (
            self.origin_x,
            self.origin_y,
            self.origin_x + (self.cols.saturating_sub(1)) as f32 * self.cell_m,
            self.origin_y + (self.rows.saturating_sub(1)) as f32 * self.cell_m,
        )
    }

    pub fn contains(&self, x: f32, y: f32) -> bool {
        let (min_x, min_y, max_x, max_y) = self.bounds();
        (min_x..=max_x).contains(&x) && (min_y..=max_y).contains(&y)
    }
}

impl DemFile {
    /// The near grid where it covers the point, the far grid otherwise.
    /// The two agree where they overlap — they are resampled from the
    /// same posts — so the join is not visible.
    pub fn height_at(&self, x: f32, y: f32) -> f32 {
        if self.inner.contains(x, y) {
            self.inner.height_at(x, y)
        } else {
            self.outer.height_at(x, y)
        }
    }
}

#[derive(Debug, thiserror::Error)]
pub enum DemError {
    #[error("io error: {0}")]
    Io(#[from] std::io::Error),
    #[error("msgpack parse error: {0}")]
    Decode(#[from] rmp_serde::decode::Error),
    #[error("elevation sidecar version {0} is newer than this build understands ({DEM_VERSION})")]
    Version(u32),
}

/// `content/tracks/real/Monza.yaml` -> `content/tracks/real/Monza.dem.msgpack`.
pub fn dem_path_for<P: AsRef<Path>>(track_path: P) -> PathBuf {
    let path = track_path.as_ref();
    let stem = path.file_stem().unwrap_or_default().to_string_lossy();
    path.with_file_name(format!("{stem}.dem.msgpack"))
}

/// Load a track's elevation sidecar. `Ok(None)` when it simply has none,
/// which is not an error: the ground falls back to the centerline blanket.
pub fn load_dem<P: AsRef<Path>>(path: P) -> Result<Option<DemFile>, DemError> {
    let path = path.as_ref();
    if !path.exists() {
        return Ok(None);
    }
    let dem: DemFile = rmp_serde::from_slice(&std::fs::read(path)?)?;
    if dem.version > DEM_VERSION {
        return Err(DemError::Version(dem.version));
    }
    Ok(Some(dem))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn grid(cell_m: f32, cols: usize, rows: usize, f: impl Fn(f32, f32) -> f32) -> DemGrid {
        let mut heights = Vec::with_capacity(cols * rows);
        for r in 0..rows {
            for c in 0..cols {
                heights.push(f(c as f32 * cell_m, r as f32 * cell_m));
            }
        }
        DemGrid {
            cell_m,
            cols,
            rows,
            origin_x: 0.0,
            origin_y: 0.0,
            heights,
        }
    }

    #[test]
    fn a_plane_is_sampled_exactly() {
        // Bilinear interpolation is exact on a plane, so any error here
        // is an indexing mistake rather than a rounding one.
        let g = grid(10.0, 20, 20, |x, y| 3.0 + 0.1 * x - 0.2 * y);
        for (x, y) in [(0.0, 0.0), (55.0, 35.0), (12.5, 7.5), (190.0, 190.0)] {
            let want = 3.0 + 0.1 * x - 0.2 * y;
            assert!(
                (g.height_at(x, y) - want).abs() < 1e-3,
                "at {x},{y}: {} vs {want}",
                g.height_at(x, y)
            );
        }
    }

    #[test]
    fn rows_and_columns_are_not_transposed() {
        // The one mistake that silently produces a plausible but wrong
        // landscape: a grid that is not square hides it, so this uses one
        // that is not.
        let g = grid(10.0, 5, 9, |x, y| x * 100.0 + y);
        assert!((g.height_at(40.0, 0.0) - 4000.0).abs() < 1e-3);
        assert!((g.height_at(0.0, 80.0) - 80.0).abs() < 1e-3);
    }

    #[test]
    fn queries_outside_clamp_to_the_border() {
        let g = grid(10.0, 10, 10, |x, _| x);
        assert!((g.height_at(-500.0, 0.0) - 0.0).abs() < 1e-3);
        assert!((g.height_at(5000.0, 0.0) - 90.0).abs() < 1e-3);
    }

    #[test]
    fn the_near_grid_wins_where_it_reaches() {
        let dem = DemFile {
            version: 1,
            source: String::new(),
            inner: grid(10.0, 11, 11, |_, _| 1.0),
            outer: grid(90.0, 11, 11, |_, _| 500.0),
        };
        // Inside the inner grid's 100 m square.
        assert!((dem.height_at(50.0, 50.0) - 1.0).abs() < 1e-3);
        // Outside it, the far grid answers.
        assert!((dem.height_at(500.0, 500.0) - 500.0).abs() < 1e-3);
    }
}
