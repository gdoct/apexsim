//! Integration tests for the `.ats` scene layer against every real track
//! shipped in `content/tracks`: a default scene can be created, saved,
//! reloaded losslessly, and repeated saves are byte-identical (the
//! determinism bar in `TRACK_EDITOR.md`). The source YAML is never written.

use std::fs;
use std::path::{Path, PathBuf};

use track_editor::ats::AtsScene;
use track_editor::ats_io;
use track_editor::track_io;
use track_editor::track_path::CenterlinePath;

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
    let mut paths: Vec<PathBuf> = fs::read_dir(&dir)
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

#[test]
fn every_real_track_gets_a_valid_default_scene_that_round_trips() {
    let dir = tempfile::tempdir().unwrap();

    for path in real_track_paths() {
        let track = track_io::load_track_file(&path)
            .unwrap_or_else(|e| panic!("editor failed to open {}: {e}", path.display()));
        let source_name = path.file_name().unwrap().to_string_lossy().into_owned();

        let scene = AtsScene::new_for_track(&track, &source_name);
        scene
            .validate()
            .unwrap_or_else(|e| panic!("{}: default scene invalid: {e}", path.display()));

        let ats_path = dir.path().join(format!("{source_name}.ats"));
        ats_io::save_ats(&ats_path, &scene)
            .unwrap_or_else(|e| panic!("{}: save failed: {e}", path.display()));
        let reloaded = ats_io::load_ats(&ats_path)
            .unwrap_or_else(|e| panic!("{}: reload failed: {e}", path.display()));
        assert_eq!(
            scene,
            reloaded,
            "{}: scene changed across save/reload",
            path.display()
        );

        let first = fs::read(&ats_path).unwrap();
        ats_io::save_ats(&ats_path, &reloaded).unwrap();
        let second = fs::read(&ats_path).unwrap();
        assert_eq!(
            first,
            second,
            "{}: repeated saves must be byte-identical",
            path.display()
        );
    }
}

#[test]
fn every_real_track_has_a_usable_centerline_path() {
    for path in real_track_paths() {
        let track = track_io::load_track_file(&path)
            .unwrap_or_else(|e| panic!("editor failed to open {}: {e}", path.display()));
        let centerline = CenterlinePath::from_track(&track)
            .unwrap_or_else(|| panic!("{}: no centerline path", path.display()));

        assert!(
            centerline.total_length_m() > 100.0,
            "{}: implausibly short ({} m)",
            path.display(),
            centerline.total_length_m()
        );
        // Station anchors must resolve to finite poses everywhere.
        let total = centerline.total_length_m();
        for i in 0..=20 {
            let s = total * (i as f32 / 20.0);
            let sample = centerline.sample_at(s);
            assert!(
                sample.pos.0.is_finite() && sample.pos.1.is_finite() && sample.pos.2.is_finite(),
                "{}: non-finite sample at station {s}",
                path.display()
            );
            assert!(sample.width_left_m > 0.0 && sample.width_right_m > 0.0);
        }
    }
}

/// The `.ats` sidecar path never collides with the YAML source: opening a
/// track and saving its scene must leave the YAML byte-identical. (The
/// editor has no code path that writes YAML anymore; this guards the file
/// layout convention.)
#[test]
fn saving_a_scene_never_touches_the_yaml_source() {
    let dir = tempfile::tempdir().unwrap();
    let source = real_track_paths().remove(0);
    let yaml_copy = dir.path().join(source.file_name().unwrap());
    fs::copy(&source, &yaml_copy).unwrap();
    let yaml_before = fs::read(&yaml_copy).unwrap();

    let track = track_io::load_track_file(&yaml_copy).unwrap();
    let scene = AtsScene::new_for_track(&track, &yaml_copy.file_name().unwrap().to_string_lossy());
    let ats_path = ats_io::ats_path_for(&yaml_copy);
    assert_ne!(ats_path, yaml_copy);
    ats_io::save_ats(&ats_path, &scene).unwrap();

    assert_eq!(fs::read(&yaml_copy).unwrap(), yaml_before);
    assert!(ats_path.exists());
}
