//! Grooming against the shipped circuits: a second pass over a groomed
//! scene must be a no-op on every real track (the generated passes are
//! cell-keyed and recognise their own output, so this is where a bounce
//! between two layouts at a folded section would show), and no stand or
//! building may be left with any part of its footprint on a road.

use std::path::{Path, PathBuf};

use track_core::ats::PropKind;
use track_core::groom::{groom_scene, stand_road_clearance_m};
use track_core::project::open_project;
use track_core::track_path::CenterlinePath;
use track_core::ue_export_io::track_files_in;

fn real_track_paths() -> Vec<PathBuf> {
    let dir = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../content/tracks/real");
    let mut found = track_files_in(&dir).expect("content/tracks/real is readable");
    found.sort();
    assert!(!found.is_empty(), "no tracks under {}", dir.display());
    found
}

#[test]
fn grooming_every_real_track_twice_changes_nothing_the_second_time() {
    for path in real_track_paths() {
        let opened = open_project(&path).unwrap();
        let mut scene = opened.scene.expect("scene loads");
        groom_scene(&opened.track, &mut scene).expect("centerline is usable");
        let after_first = scene.clone();
        let second = groom_scene(&opened.track, &mut scene).unwrap();
        assert!(
            !second.changed(),
            "{}: second groom changed the scene: {second:?}",
            path.display()
        );
        assert_eq!(scene, after_first, "{}", path.display());
        scene
            .validate()
            .unwrap_or_else(|e| panic!("{}: {e}", path.display()));
    }
}

#[test]
fn no_groomed_stand_or_building_reaches_a_road() {
    for path in real_track_paths() {
        let opened = open_project(&path).unwrap();
        let mut scene = opened.scene.expect("scene loads");
        groom_scene(&opened.track, &mut scene).unwrap();
        let centerline = CenterlinePath::from_track(&opened.track).unwrap();
        for prop in scene
            .props
            .iter()
            .filter(|p| matches!(p.kind, PropKind::Grandstand | PropKind::Building))
        {
            let clearance = stand_road_clearance_m(&centerline, prop).unwrap();
            assert!(
                clearance >= 3.0 - 0.01,
                "{}: prop {} ({:?}, scale {}) clears the road by only {clearance:.2} m",
                path.display(),
                prop.id,
                prop.kind,
                prop.scale
            );
        }
    }
}
