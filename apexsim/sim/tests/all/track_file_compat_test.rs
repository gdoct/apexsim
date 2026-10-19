//! A track YAML rewritten by a content tool loads to the same physical
//! track.
//!
//! The editor and the `ats-*` tools rewrite a track by parsing it into the
//! formats crate's `TrackFile` and writing it back with `serde_yaml` (and
//! JSON for a `.json` file), the type the server parses it with too. This
//! holds every real track under `apexsim-content/tracks/default` to it: the
//! rewritten file builds the same centerline, grid, checkpoints, sectors and
//! racing line through the server's own `TrackLoader` as the original.

use std::path::{Path, PathBuf};

use apexsim_formats::track::TrackFile;
use apexsim_sim::data::TrackConfig;
use apexsim_sim::track_loader::TrackLoader;

fn content_tracks_dir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../apexsim-content/tracks/default")
}

/// Every `<dir>/<Stem>/<Stem>.yaml`, sorted.
fn real_track_paths() -> Vec<PathBuf> {
    let dir = content_tracks_dir();
    let mut found: Vec<PathBuf> = std::fs::read_dir(&dir)
        .unwrap_or_else(|e| panic!("failed to read {}: {e}", dir.display()))
        .filter_map(Result::ok)
        .map(|e| e.path())
        .filter(|p| p.is_dir())
        .filter_map(|p| {
            let yaml = p.join(format!("{}.yaml", p.file_name()?.to_str()?));
            yaml.is_file().then_some(yaml)
        })
        .collect();
    found.sort();
    assert!(
        !found.is_empty(),
        "expected at least one real track under {}",
        dir.display()
    );
    found
}

/// Assert that two `TrackConfig`s describe the same physical track: same
/// centerline geometry/surface/grip, same start grid, same checkpoints.
///
/// `has_declared_track_id` is false for a source file with no `track_id`:
/// `TrackLoader` gives such a track a fresh id on every load, so two loads
/// of the same file disagree on `id` by design.
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
        original.sectors, roundtripped.sectors,
        "sectors (resolved stations)"
    );
    assert_eq!(original.raceline, roundtripped.raceline, "raceline");
    assert_eq!(original.drs_zones, roundtripped.drs_zones, "drs zones");
    assert_eq!(original.metadata, roundtripped.metadata, "metadata");

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
fn a_rewritten_track_yaml_loads_to_the_same_track() {
    for path in real_track_paths() {
        let text = std::fs::read_to_string(&path).unwrap();
        let original = TrackLoader::load_from_string(&text)
            .unwrap_or_else(|e| panic!("server failed to load {}: {e}", path.display()));

        let track: TrackFile = serde_yaml::from_str(&text)
            .unwrap_or_else(|e| panic!("{} does not parse: {e}", path.display()));
        for (form, rewritten) in [
            ("yaml", serde_yaml::to_string(&track).unwrap()),
            ("json", serde_json::to_string_pretty(&track).unwrap()),
        ] {
            let back = TrackLoader::load_from_string(&rewritten).unwrap_or_else(|e| {
                panic!(
                    "server failed to load the rewritten {form} of {}: {e}",
                    path.display()
                )
            });
            assert_physically_identical(&original, &back, track.track_id.is_some());
        }
    }
}
