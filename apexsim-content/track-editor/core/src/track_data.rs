//! Logical track data model: the track YAML.
//!
//! The file's types are modelled once, in `apexsim-formats`
//! (`apexsim-lib/formats/src/track.rs`), which the server reads it with
//! too; this module re-exports them under the names the editor and the
//! `ats-*` tools use. A key added to the YAML goes in the crate, and every
//! tool that rewrites a YAML writes it through [`TrackFile`].

pub use apexsim_formats::track::{
    Checkpoint, DrsZone, RacelinePoint, SpawnPoint, TrackFile, TrackMetadata, TrackNode,
};
