//! The formats the content writes and the code reads.
//!
//! The track editor's `track-core` (`apexsim-content`) writes these files and
//! the server (`apexsim`) reads them; both build against this crate, which
//! depends on neither. Each format is modelled once, here: the serialized
//! types, their version numbers and the sidecar paths. What either side
//! does with them (the bake, the sim's queries) stays with that side.
//!
//! - [`track`]: the track YAML (`<Stem>.yaml`), the authored source.
//! - [`ground`], [`curbs`], [`walls`], [`road_mesh`], [`pit`]: the
//!   sidecars `ats-export` bakes beside the YAML (`<Stem>.<kind>.msgpack`,
//!   written with `rmp_serde::to_vec_named`).
//! - [`content_crc`]: the checksum both sides put on a source file.
//!
//! Serde attributes are load-bearing: the tools rewrite YAMLs through
//! [`track::TrackFile`], so a changed attribute changes the bytes of every
//! rewritten file, and with them its content checksum.

pub mod content_crc;
pub mod curbs;
pub mod ground;
pub mod pit;
pub mod road_mesh;
pub mod track;
pub mod walls;

use std::path::{Path, PathBuf};

// Format levels.
//
// A format level is what a pack states its files are written in and what a
// reader states it can read (`apexsim/docs/proposals/split-off-content.md`,
// "Version rules"). A level changes only when a reader of the old level
// cannot read the new files: a new key an old reader ignores, or one that is
// `#[serde(default)]` for a new reader of old files, does not raise it.

/// Level of the track YAML ([`track::TrackFile`]).
pub const TRACK_YAML_LEVEL: u32 = 1;

/// Level of the baked sidecars as a set (ground, curbs, walls, road mesh,
/// pit lane). Each file also carries its own `version` (the `*_VERSION`
/// constants), which its reader checks.
pub const SIDECARS_LEVEL: u32 = 1;

/// `version` of [`ground::GroundHeightfield`].
pub const GROUND_VERSION: u32 = ground::GROUND_SIDECAR_VERSION;
/// `version` of [`curbs::CurbBands`] as written; a reader takes 1 to this.
pub const CURB_BANDS_VERSION: u32 = curbs::CURB_BANDS_VERSION;
/// `version` of [`walls::WallFile`].
pub const WALLS_VERSION: u32 = walls::WALLS_VERSION;
/// `version` of [`road_mesh::RoadMeshFile`].
pub const ROAD_MESH_VERSION: u32 = road_mesh::ROAD_MESH_VERSION;
/// `version` of [`pit::PitSidecar`].
pub const PIT_SIDECAR_VERSION: u32 = pit::PIT_SIDECAR_VERSION;

/// The path of a track's sidecar of `kind` (`ground`, `curbs`, `walls`,
/// `road`, `pit`): beside the track file, its stem with
/// `.<kind>.msgpack` (`Monzarella.yaml` -> `Monzarella.ground.msgpack`).
pub fn sidecar_path(track_file: &Path, kind: &str) -> PathBuf {
    track_file.with_extension(format!("{kind}.msgpack"))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sidecars_sit_beside_the_track_under_its_stem() {
        let track = Path::new("tracks/default/Monzarella/Monzarella.yaml");
        assert_eq!(
            sidecar_path(track, "ground"),
            Path::new("tracks/default/Monzarella/Monzarella.ground.msgpack")
        );
        assert_eq!(
            road_mesh::RoadMeshFile::sidecar_path(track),
            Path::new("tracks/default/Monzarella/Monzarella.road.msgpack")
        );
        assert!(pit::PitSidecar::sidecar_path(track).ends_with("Monzarella.pit.msgpack"));
        assert!(walls::WallFile::sidecar_path(track).ends_with("Monzarella.walls.msgpack"));
        assert!(curbs::CurbBands::sidecar_path(track).ends_with("Monzarella.curbs.msgpack"));
        assert!(ground::GroundHeightfield::sidecar_path(track).ends_with("Monzarella.ground.msgpack"));
    }
}
