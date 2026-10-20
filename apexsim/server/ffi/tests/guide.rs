//! A track guide made through the C API is the one `apexsim-replay guide`
//! writes with its defaults: both run `apexsim_protocol::track_guide`. Its
//! stream carries a seeded id, so the same guide is the same bytes twice.

use std::ffi::{CStr, CString};
use std::path::{Path, PathBuf};

use apexsim_physics::*;
use apexsim_protocol::track_guide::{build_guide, write_guide, GuideOptions};

fn repo(rel: &str) -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../..")
        .join(rel)
}

const TRACK: &str = "../apexsim-content/tracks/default/Snorisring/Snorisring.yaml";
const CARS: &str = "../apexsim-content/cars/default";

fn cp(path: &Path) -> CString {
    CString::new(path.to_string_lossy().into_owned()).unwrap()
}

fn scratch(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("apx-guide-{name}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

fn build(track: &Path, class: &str, out: &Path, cancel: Option<&u8>) -> ApxStatus {
    let (track, cars, class, out) = (
        cp(track),
        cp(&repo(CARS)),
        CString::new(class).unwrap(),
        cp(out),
    );
    unsafe {
        apx_guide_build(
            track.as_ptr(),
            cars.as_ptr(),
            class.as_ptr(),
            out.as_ptr(),
            cancel.map_or(std::ptr::null(), |flag| flag as *const u8),
        )
    }
}

#[test]
fn a_guide_through_the_api_is_the_tools() {
    let track = repo(TRACK);
    if !track.is_file() {
        eprintln!("skipped: no content checkout beside the code");
        return;
    }
    let dir = scratch("same");
    let api = dir.join("api");
    assert_eq!(build(&track, "GT3", &api, None), APX_OK, "{:?}", unsafe {
        CStr::from_ptr(apx_last_error())
    });

    let tool = dir.join("tool");
    let built = build_guide(&GuideOptions {
        track_path: track.clone(),
        cars_dir: repo(CARS),
        class: "GT3".to_string(),
        ..GuideOptions::default()
    })
    .unwrap();
    write_guide(&built, &tool).unwrap();
    for name in ["Snorisring.GT3.guide.json", "Snorisring.GT3.guide.apxs"] {
        let (a, b) = (
            std::fs::read(api.join(name)).unwrap(),
            std::fs::read(tool.join(name)).unwrap(),
        );
        assert!(a == b, "{name} differs between the API and the tool");
    }
    // Nothing left aside.
    let left: Vec<_> = std::fs::read_dir(&api).unwrap().collect();
    assert_eq!(left.len(), 2);

    let flag = 1u8;
    let cancelled = dir.join("cancelled");
    assert_eq!(
        build(&track, "GT3", &cancelled, Some(&flag)),
        APX_ERR_CANCELLED
    );
    assert!(!cancelled.join("Snorisring.GT3.guide.json").exists());
    let _ = std::fs::remove_dir_all(&dir);
}

/// The build's guide (`build/guide/`, made by the server's
/// `apexsim-replay guide`) is what the library makes in release, both
/// files. Run after a content bake.
#[test]
#[ignore = "cargo test --release --test guide -- --ignored, after a bake"]
fn the_shipped_guide_is_made_again_through_the_api() {
    let shipped = repo("build/guide");
    let track = repo("../apexsim-content/tracks/default/Zandervoort/Zandervoort.yaml");
    if !shipped.join("Zandervoort.GT3.guide.json").is_file() || !track.is_file() {
        eprintln!("skipped: no build/guide or content");
        return;
    }
    let dir = scratch("shipped");
    assert_eq!(build(&track, "GT3", &dir, None), APX_OK);
    for name in ["Zandervoort.GT3.guide.json", "Zandervoort.GT3.guide.apxs"] {
        assert!(
            std::fs::read(dir.join(name)).unwrap() == std::fs::read(shipped.join(name)).unwrap(),
            "{name} differs from build/guide"
        );
    }
    let _ = std::fs::remove_dir_all(&dir);
}
