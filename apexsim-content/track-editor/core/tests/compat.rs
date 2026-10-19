//! Every real track opens in the editor, and a save and reopen gives the
//! same track back.
//!
//! The YAML's types are the formats crate's (`apexsim-formats`), which the
//! server reads the file with too, so a file the editor saves is a file the
//! server reads by construction. That the server builds the same physical
//! track from an editor-saved file as from the original is the server's own
//! test (`apexsim/sim/tests/all/track_file_compat_test.rs`).

use std::path::{Path, PathBuf};

use track_core::track_data::TrackFile;
use track_core::track_io;

/// Real tracks live at `<content_root>/tracks/default/<Stem>/<Stem>.yaml`; this crate
/// lives at `<content_root>/track-editor/core`.
fn content_tracks_dir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../tracks/default")
}

fn real_track_paths() -> Vec<PathBuf> {
    let dir = content_tracks_dir();
    let paths = track_core::ue_export_io::track_files_in(&dir)
        .unwrap_or_else(|e| panic!("failed to read {}: {e}", dir.display()));
    assert!(
        !paths.is_empty(),
        "expected at least one real track under {}",
        dir.display()
    );
    paths
}

#[test]
fn every_real_track_opens_in_the_editor() {
    for path in real_track_paths() {
        track_io::load_track_file(&path)
            .unwrap_or_else(|e| panic!("editor failed to open {}: {e}", path.display()));
    }
}

#[test]
fn editor_round_trip_preserves_the_track() {
    let dir = tempfile::tempdir().unwrap();

    for path in real_track_paths() {
        let track: TrackFile = track_io::load_track_file(&path)
            .unwrap_or_else(|e| panic!("editor failed to open {}: {e}", path.display()));

        let out_path = dir.path().join(path.file_name().unwrap());
        track_io::save_track_file(&out_path, &track)
            .unwrap_or_else(|e| panic!("editor failed to save {}: {e}", path.display()));

        // The editor's own model must still describe the same track after
        // a save/reload — no field silently dropped or defaulted.
        let reloaded_track = track_io::load_track_file(&out_path)
            .unwrap_or_else(|e| panic!("editor failed to reopen saved {}: {e}", path.display()));
        assert_eq!(
            track,
            reloaded_track,
            "{}: editor model changed across save/reload",
            path.display()
        );

        // And a second save writes the very bytes of the first: a rewrite
        // of a rewritten file changes nothing, content checksum included.
        let first = std::fs::read(&out_path).unwrap();
        track_io::save_track_file(&out_path, &reloaded_track).unwrap();
        assert_eq!(
            first,
            std::fs::read(&out_path).unwrap(),
            "{}: a second save changed the bytes",
            path.display()
        );
    }
}
