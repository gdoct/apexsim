//! A showcase rendered through the C API is the file `apexsim-replay
//! showcase` writes for the same entry of the same list: both read the list
//! and render the race in `apexsim-protocol` (`showcase_list`, `render`).
//! A raised cancel flag stops the render with nothing written, and the list
//! names its showcases.

use std::ffi::{CStr, CString};
use std::path::{Path, PathBuf};

use apexsim_physics::*;
use apexsim_protocol::showcase_list::ShowcaseList;

fn repo(rel: &str) -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../..")
        .join(rel)
}

const TRACKS: &str = "../apexsim-content/tracks";
const CARS: &str = "../apexsim-content/cars/default";

/// A short race, so a debug build renders it in seconds.
const LIST: &str = r#"
defaults:
  cars: 4
  laps: 1
  seed: 3
variants:
  day:   { weather: sunny, time: "13:00" }
  dusk:  { weather: cloudy, time: "19:30" }
showcases:
  - { track: Zandervoort, class: GT3, variants: [day, dusk] }
"#;

fn c(text: &str) -> CString {
    CString::new(text).unwrap()
}

fn cp(path: &Path) -> CString {
    c(&path.to_string_lossy())
}

fn last_error() -> String {
    unsafe { CStr::from_ptr(apx_last_error()) }
        .to_string_lossy()
        .into_owned()
}

fn write_list(dir: &Path) -> PathBuf {
    let path = dir.join("showcase.yml");
    std::fs::write(&path, LIST).unwrap();
    path
}

fn scratch(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("apx-showcase-{name}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    dir
}

fn render(list: &Path, name: &str, out: &Path, cancel: Option<&u8>) -> ApxStatus {
    let (list, tracks, cars, name, out) = (
        cp(list),
        cp(&repo(TRACKS)),
        cp(&repo(CARS)),
        c(name),
        cp(out),
    );
    unsafe {
        apx_showcase_render(
            list.as_ptr(),
            tracks.as_ptr(),
            cars.as_ptr(),
            name.as_ptr(),
            out.as_ptr(),
            cancel.map_or(std::ptr::null(), |flag| flag as *const u8),
        )
    }
}

#[test]
fn a_showcase_through_the_api_is_the_tools_file() {
    if !repo(TRACKS)
        .join("default/Zandervoort/Zandervoort.yaml")
        .is_file()
    {
        eprintln!("skipped: no content checkout beside the code");
        return;
    }
    let dir = scratch("same");
    let list = write_list(&dir);
    let out = dir.join("Zandervoort.gt3.day.apxs");
    assert_eq!(
        render(&list, "Zandervoort.gt3.day", &out, None),
        APX_OK,
        "{}",
        last_error()
    );
    assert!(!out.with_extension("apxs.part").exists());

    let tool = ShowcaseList::load(&list)
        .unwrap()
        .render("Zandervoort.gt3.day", &repo(TRACKS), &repo(CARS))
        .unwrap()
        .run()
        .unwrap();
    let tool_out = dir.join("tool.apxs");
    tool.content.write_file(&tool_out).unwrap();
    let (api, tool) = (
        std::fs::read(&out).unwrap(),
        std::fs::read(&tool_out).unwrap(),
    );
    assert!(api.len() > 10_000, "{} bytes", api.len());
    assert!(api == tool, "the API's file differs from the tool's");

    // Another variant is another race (the sky differs).
    let dusk = dir.join("dusk.apxs");
    assert_eq!(render(&list, "Zandervoort.gt3.dusk", &dusk, None), APX_OK);
    assert!(std::fs::read(&dusk).unwrap() != api);
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn a_raised_cancel_flag_writes_nothing() {
    if !repo(TRACKS)
        .join("default/Zandervoort/Zandervoort.yaml")
        .is_file()
    {
        return;
    }
    let dir = scratch("cancel");
    let list = write_list(&dir);
    let out = dir.join("cancelled.apxs");
    let flag = 1u8;
    assert_eq!(
        render(&list, "Zandervoort.gt3.day", &out, Some(&flag)),
        APX_ERR_CANCELLED
    );
    assert!(!out.exists() && !out.with_extension("apxs.part").exists());
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn the_list_names_its_showcases_and_refuses_an_unknown_one() {
    let dir = scratch("names");
    let list = write_list(&dir);
    let path = cp(&list);
    let mut written = 0usize;
    let status =
        unsafe { apx_showcase_names(path.as_ptr(), std::ptr::null_mut(), 0, &mut written) };
    assert_eq!(status, APX_ERR_BUFFER);
    let mut buf = vec![0u8; written];
    let status =
        unsafe { apx_showcase_names(path.as_ptr(), buf.as_mut_ptr(), buf.len(), &mut written) };
    assert_eq!(status, APX_OK);
    assert_eq!(
        String::from_utf8(buf).unwrap(),
        "Zandervoort.gt3.day\nZandervoort.gt3.dusk"
    );

    let out = dir.join("none.apxs");
    assert_ne!(render(&list, "Nowhere.gt3.day", &out, None), APX_OK);
    assert!(last_error().contains("Nowhere.gt3.day"), "{}", last_error());
    let _ = std::fs::remove_dir_all(&dir);
}

/// The real list's Zandervoort showcase through the API is the file the
/// build rendered with the server's `apexsim-replay` (`build/showcase/`):
/// the two builds agree byte for byte. A whole 20-car race: run it in
/// release, after a content bake.
#[test]
#[ignore = "renders a whole showcase; cargo test --release --test showcase -- --ignored"]
fn the_shipped_showcase_renders_again_through_the_api() {
    let shipped = repo("build/showcase/Zandervoort.gt3.day.apxs");
    if !shipped.is_file() {
        eprintln!(
            "skipped: no {} (render the showcases first)",
            shipped.display()
        );
        return;
    }
    let dir = scratch("shipped");
    let out = dir.join("Zandervoort.gt3.day.apxs");
    assert_eq!(
        render(&repo("showcase.yml"), "Zandervoort.gt3.day", &out, None),
        APX_OK,
        "{}",
        last_error()
    );
    assert!(
        std::fs::read(&out).unwrap() == std::fs::read(&shipped).unwrap(),
        "the API's render differs from {}",
        shipped.display()
    );
    let _ = std::fs::remove_dir_all(&dir);
}
