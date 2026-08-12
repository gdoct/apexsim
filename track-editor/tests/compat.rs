//! Compatibility tests between the editor's logical-track model and the
//! server's own loader (`apexsim_server::track_loader::TrackLoader`).
//!
//! `track-editor` deliberately does not depend on `apexsim-server` at
//! runtime (the server crate pulls in tokio/hyper/rustls, which the editor
//! has no use for and shouldn't pay compile time for). This test suite is
//! the substitute the spec calls for: it proves, against every real track
//! shipped in `content/tracks`, that a file loaded and re-saved by the
//! editor still loads through the server's `TrackLoader` and produces the
//! same physical track — the acceptance bar in `TRACK_EDITOR.md` #4 and #8.

use std::path::{Path, PathBuf};

use apexsim_server::data::TrackConfig;
use apexsim_server::track_loader::TrackLoader;

use track_editor::track_data::TrackFile;
use track_editor::track_io;

/// Real tracks live at `<repo_root>/content/tracks/real/*.yaml`; this crate
/// lives at `<repo_root>/track-editor`.
fn content_tracks_dir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .expect("track-editor has a parent directory")
        .join("content/tracks/real")
}

fn real_track_paths() -> Vec<PathBuf> {
    let dir = content_tracks_dir();
    let mut paths: Vec<PathBuf> = std::fs::read_dir(&dir)
        .unwrap_or_else(|e| panic!("failed to read {}: {e}", dir.display()))
        .filter_map(|entry| entry.ok())
        .map(|entry| entry.path())
        .filter(|p| p.extension().and_then(|e| e.to_str()) == Some("yaml"))
        .collect();
    paths.sort();
    assert!(
        !paths.is_empty(),
        "expected at least one real track under {}",
        dir.display()
    );
    paths
}

/// Assert that two `TrackConfig`s describe the same physical track: same
/// centerline geometry/surface/grip, same start grid, same checkpoints. Does
/// NOT compare `source_path`, which legitimately differs between a load from
/// the original file and a load from a re-saved copy.
///
/// `has_declared_track_id` should be false for a source file with no
/// `track_id` field: `TrackLoader` assigns such tracks a fresh random UUID
/// on every load, by design, so two independent loads of the *same* file
/// legitimately disagree on `id`. That's a pre-existing server behavior
/// (not an editor round-trip bug) and out of scope for this test.
fn assert_physically_identical(
    original: &TrackConfig,
    roundtripped: &TrackConfig,
    has_declared_track_id: bool,
) {
    if has_declared_track_id {
        assert_eq!(original.id, roundtripped.id, "track_id");
    }
    assert_eq!(original.name, roundtripped.name, "name");
    assert_eq!(original.width_m, roundtripped.width_m, "width_m");
    assert_eq!(
        original.checkpoints, roundtripped.checkpoints,
        "checkpoints (resolved distances)"
    );
    assert_eq!(
        original.raceline.len(),
        roundtripped.raceline.len(),
        "raceline length"
    );
    for (a, b) in original.raceline.iter().zip(&roundtripped.raceline) {
        assert_eq!((a.x, a.y, a.z), (b.x, b.y, b.z), "raceline point");
    }

    assert_eq!(
        original.start_positions.len(),
        roundtripped.start_positions.len(),
        "start grid size"
    );
    for (a, b) in original
        .start_positions
        .iter()
        .zip(&roundtripped.start_positions)
    {
        assert_eq!(a.position, b.position, "start slot position");
        assert_eq!(
            (a.x, a.y, a.z, a.yaw_rad),
            (b.x, b.y, b.z, b.yaw_rad),
            "start slot pose"
        );
    }

    assert_eq!(
        original.centerline.len(),
        roundtripped.centerline.len(),
        "centerline point count"
    );
    for (i, (a, b)) in original
        .centerline
        .iter()
        .zip(&roundtripped.centerline)
        .enumerate()
    {
        assert_eq!((a.x, a.y, a.z), (b.x, b.y, b.z), "centerline[{i}] position");
        assert_eq!(
            (a.width_left_m, a.width_right_m),
            (b.width_left_m, b.width_right_m),
            "centerline[{i}] width"
        );
        assert_eq!(a.banking_rad, b.banking_rad, "centerline[{i}] banking_rad");
        assert_eq!(
            a.surface_type, b.surface_type,
            "centerline[{i}] surface_type"
        );
        assert_eq!(
            a.grip_modifier, b.grip_modifier,
            "centerline[{i}] grip_modifier"
        );
    }
}

#[test]
fn every_real_track_opens_in_the_editor() {
    for path in real_track_paths() {
        track_io::load_track_file(&path)
            .unwrap_or_else(|e| panic!("editor failed to open {}: {e}", path.display()));
    }
}

#[test]
fn editor_round_trip_preserves_server_load() {
    let dir = tempfile::tempdir().unwrap();

    for path in real_track_paths() {
        let original_config = TrackLoader::load_from_file(&path)
            .unwrap_or_else(|e| panic!("server failed to load {}: {e}", path.display()));

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

        // And the server must still load the editor-saved file to an
        // identical physical track.
        let roundtripped_config = TrackLoader::load_from_file(&out_path).unwrap_or_else(|e| {
            panic!(
                "server failed to load editor-saved {}: {e}",
                out_path.display()
            )
        });

        assert_physically_identical(
            &original_config,
            &roundtripped_config,
            track.track_id.is_some(),
        );
    }
}
