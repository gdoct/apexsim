//! The dashboard's Content view: cars and tracks switched off, and new ones
//! uploaded into `custom/` while the server runs.
//!
//! A switched-off car or track stays loaded (a session already on it runs
//! on) but the lobby leaves it out, a select or create naming it is
//! refused, and the AI is never dealt it. The set is kept in
//! `[admin] content_file`.
//!
//! An upload is one file per request. A car is its `car.toml`, written to
//! `<cars_dir>/custom/<car name>/car.toml`; a track is its YAML and any
//! sidecars beside it (`<Stem>.road.msgpack` and the rest from
//! `ats-export`), all written to `<tracks_dir>/custom/<Stem>/`. The
//! sidecars go first: the YAML is what puts the track in the catalog, and a
//! track whose sidecars change is read again by the next session on it.
//! What the server cannot parse is refused before it replaces anything, and
//! an upload never takes a shipped car's or track's id.

use crate::server::ServerState;
use serde_json::json;
use std::collections::BTreeSet;
use std::path::{Path, PathBuf};
use tracing::{info, warn};
use uuid::Uuid;

/// The switched-off ids kept in `path`; none when the file is absent or
/// unreadable (logged).
pub fn load_disabled(path: &str) -> BTreeSet<Uuid> {
    if path.trim().is_empty() {
        return BTreeSet::new();
    }
    let Ok(bytes) = std::fs::read(path) else {
        return BTreeSet::new();
    };
    match serde_json::from_slice::<serde_json::Value>(&bytes) {
        Ok(v) => v
            .get("disabled")
            .and_then(|d| d.as_array())
            .map(|ids| {
                ids.iter()
                    .filter_map(|id| id.as_str().and_then(|s| s.parse().ok()))
                    .collect()
            })
            .unwrap_or_default(),
        Err(e) => {
            warn!(
                "{} is not readable ({}); no content is switched off",
                path, e
            );
            BTreeSet::new()
        }
    }
}

/// Write the switched-off ids, beside and renamed so a crash never leaves
/// half a file.
pub fn save_disabled(path: &str, ids: &BTreeSet<Uuid>) -> std::io::Result<()> {
    if path.trim().is_empty() {
        return Ok(());
    }
    let text =
        serde_json::to_string_pretty(&json!({ "disabled": ids })).map_err(std::io::Error::other)?;
    write_atomic(Path::new(path), text.as_bytes())
}

fn write_atomic(path: &Path, bytes: &[u8]) -> std::io::Result<()> {
    if let Some(dir) = path.parent().filter(|d| !d.as_os_str().is_empty()) {
        std::fs::create_dir_all(dir)?;
    }
    let mut tmp = path.as_os_str().to_os_string();
    tmp.push(".upload");
    let tmp = PathBuf::from(tmp);
    std::fs::write(&tmp, bytes)?;
    std::fs::rename(&tmp, path).inspect_err(|_| {
        let _ = std::fs::remove_file(&tmp);
    })
}

/// A folder or file name made safe to join to a path: letters, digits,
/// `-`, `_`, `.` and spaces (anything else becomes `_`), never `.` or
/// `..`, at most 64 characters.
fn safe_name(name: &str) -> Option<String> {
    let cleaned: String = name
        .trim()
        .chars()
        .map(|c| {
            if c.is_ascii_alphanumeric() || matches!(c, '-' | '_' | '.' | ' ') {
                c
            } else {
                '_'
            }
        })
        .take(64)
        .collect();
    let cleaned = cleaned.trim().trim_matches('.').to_string();
    (!cleaned.is_empty()).then_some(cleaned)
}

/// Whether `path` lies under `<dir>/custom`, compared as the loader
/// wrote it (separators normalised).
fn in_custom(path: &str) -> bool {
    path.replace('\\', "/").contains("/custom/") || path.replace('\\', "/").starts_with("custom/")
}

/// The state as an upload uses it: read for the folders, written only to
/// put the new content in, never across a disk write.
type State = tokio::sync::RwLock<ServerState>;

/// Upload a car's `car.toml`. Returns the car's id and name. Blocking: run
/// it off the runtime.
pub fn upload_car(state: &State, bytes: &[u8]) -> Result<serde_json::Value, String> {
    let text = std::str::from_utf8(bytes).map_err(|_| "car.toml is not UTF-8 text")?;
    // Parsed from a scratch file first: nothing is replaced until it loads.
    let probe = std::env::temp_dir().join(format!("apexsim-upload-{}.car.toml", Uuid::new_v4()));
    std::fs::write(&probe, text).map_err(|e| e.to_string())?;
    let loaded = crate::car_loader::CarLoader::load_from_file(&probe);
    let _ = std::fs::remove_file(&probe);
    let car = loaded.map_err(|e| e.to_string())?;

    let folder = safe_name(&car.name).ok_or("the car has no usable name")?;
    let cars_dir = state.blocking_read().config.content.cars_dir.clone();
    let target = Path::new(&cars_dir)
        .join("custom")
        .join(&folder)
        .join("car.toml");
    // The same custom car again replaces it; any other car keeps its id.
    let replaces_itself = crate::car_loader::CarLoader::load_from_file(&target)
        .is_ok_and(|on_disk| on_disk.id == car.id);
    let mut st = state.blocking_write();
    if let Some(existing) = st.car_configs.get(&car.id).filter(|_| !replaces_itself) {
        return Err(format!(
            "a car with this id is already loaded ({}); give the new car its own id",
            existing.name
        ));
    }
    // A car.toml is a few kilobytes: written under the lock, so a second
    // upload of the same id cannot slip in between.
    write_atomic(&target, text.as_bytes())
        .map_err(|e| format!("cannot write {}: {}", target.display(), e))?;
    info!(
        "Admin uploaded car {} ({}) to {}",
        car.name,
        car.id,
        target.display()
    );
    let reply = json!({ "id": car.id, "name": car.name, "file": target.to_string_lossy() });
    st.car_configs.insert(car.id, car);
    Ok(reply)
}

/// Upload one file of a track: its YAML (which puts it in the catalog) or
/// a sidecar named after it. Blocking: run it off the runtime.
pub fn upload_track_file(
    state: &State,
    file_name: &str,
    bytes: &[u8],
) -> Result<serde_json::Value, String> {
    let name = safe_name(file_name).ok_or("a file name is needed")?;
    if name != file_name.trim() {
        return Err("the file name may only hold letters, digits, '-', '_', '.' and spaces".into());
    }
    let stem = name.split('.').next().unwrap_or_default().to_string();
    if stem.is_empty() || stem == name {
        return Err("a track file is named <Track>.yaml or <Track>.<sidecar>".into());
    }
    let tracks_dir = PathBuf::from(&state.blocking_read().config.content.tracks_dir);
    let dir = tracks_dir.join("custom").join(&stem);
    let target = dir.join(&name);
    let yaml = dir.join(format!("{stem}.yaml"));
    let lower = name.to_ascii_lowercase();
    let is_track =
        (lower.ends_with(".yaml") || lower.ends_with(".yml")) && lower.matches('.').count() == 1;

    if !is_track {
        write_atomic(&target, bytes)
            .map_err(|e| format!("cannot write {}: {}", target.display(), e))?;
        // A track already loaded from this folder reads its sidecars again
        // for its next session.
        let st = state.blocking_read();
        let suffix = format!("/{stem}/{stem}.yaml");
        let loaded = st
            .track_configs
            .values()
            .find(|t| {
                t.source_path
                    .as_deref()
                    .is_some_and(|p| in_custom(p) && p.ends_with(&suffix))
            })
            .map(|t| t.id);
        if let Some(id) = loaded {
            st.track_content.register(id, yaml);
        }
        info!(
            "Admin uploaded {} ({} bytes)",
            target.display(),
            bytes.len()
        );
        return Ok(json!({ "file": target.to_string_lossy(), "track": null }));
    }

    let text = std::str::from_utf8(bytes).map_err(|_| "the track file is not UTF-8 text")?;
    // Parsed where it will live (the loader reads files beside it), the old
    // file put back if it does not load or takes another track's id.
    let previous = std::fs::read(&target).ok();
    write_atomic(&target, text.as_bytes())
        .map_err(|e| format!("cannot write {}: {}", target.display(), e))?;
    let restore = |why: String| -> Result<serde_json::Value, String> {
        match &previous {
            Some(old) => {
                let _ = write_atomic(&target, old);
            }
            None => {
                let _ = std::fs::remove_file(&target);
            }
        }
        Err(why)
    };
    let mut track = match crate::track_loader::TrackLoader::load_catalog_entry(&target) {
        Ok(t) => t,
        Err(e) => return restore(e.to_string()),
    };
    let content_root = tracks_dir.parent().unwrap_or(&tracks_dir);
    let rel = target
        .strip_prefix(content_root)
        .unwrap_or(&target)
        .to_string_lossy()
        .replace('\\', "/");
    let mut st = state.blocking_write();
    if let Some(existing) = st.track_configs.get(&track.id) {
        if existing.source_path.as_deref() != Some(rel.as_str()) {
            let why = format!(
                "a track with this track_id is already loaded ({}); give the new track its own track_id",
                existing.name
            );
            drop(st);
            return restore(why);
        }
    }
    track.source_path = Some(rel);
    st.track_content.register(track.id, target.clone());
    info!(
        "Admin uploaded track {} ({}) to {}",
        track.name,
        track.id,
        target.display()
    );
    let reply = json!({ "file": target.to_string_lossy(), "track": { "id": track.id, "name": track.name } });
    st.track_configs.insert(track.id, track);
    Ok(reply)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn safe_names_keep_plain_names_and_refuse_paths() {
        assert_eq!(safe_name("Spa.yaml").as_deref(), Some("Spa.yaml"));
        assert_eq!(safe_name("My Car").as_deref(), Some("My Car"));
        assert_eq!(safe_name("../x").as_deref(), Some("_x"));
        assert_eq!(safe_name("a/b\\c").as_deref(), Some("a_b_c"));
        assert_eq!(safe_name(".."), None);
        assert_eq!(safe_name("   "), None);
    }

    #[test]
    fn the_switched_off_set_survives_a_save_and_a_load() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("sub").join("content.json");
        let path = path.to_string_lossy().into_owned();
        assert!(load_disabled(&path).is_empty());
        let ids: BTreeSet<Uuid> = [Uuid::from_u128(1), Uuid::from_u128(7)].into();
        save_disabled(&path, &ids).unwrap();
        assert_eq!(load_disabled(&path), ids);
    }
}
