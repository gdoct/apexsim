//! A track's sidecars, loaded the first time a session needs them.
//!
//! Startup parses every track file (`TrackLoader::load_catalog_entry`), which
//! is all the lobby shows, and leaves the baked sidecars on disk: the road
//! meshes alone are hundreds of megabytes across the calendar, and reading
//! every one took a debug server 20 s to start. The first session on a
//! track loads its sidecars here (`TrackLoader::load_sidecars`), and the
//! complete track is kept for every session after it.
//!
//! The game loop loads a track off the loop before it creates the session
//! (`game_loop::track_loads`), so a 240 Hz session already running never
//! waits for a disk read; [`TrackContent::complete`] also loads in place for
//! a caller that has not, such as a test driving `ServerState` directly.

use crate::config::RoadContactMode;
use crate::content_crc::content_crc;
use crate::data::{TrackConfig, TrackConfigId};
use crate::track_bundle::{TrackBundle, TrackFiles};
use crate::track_loader::TrackLoader;
use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::{Arc, OnceLock, RwLock};
use tracing::{info, warn};

pub struct TrackContent {
    road_contact: RoadContactMode,
    /// Behind a lock so a track uploaded on the dashboard can be
    /// registered while the server runs; a slot is cloned out before any
    /// load, so the lock is never held across a disk read.
    tracks: RwLock<HashMap<TrackConfigId, Arc<Slot>>>,
}

struct Slot {
    /// The track file, where its sidecars sit beside it.
    path: PathBuf,
    complete: OnceLock<Loaded>,
}

/// A track with its sidecars, and the files it was loaded from as the
/// bundle a predicting client asks for (`crate::track_bundle`).
#[derive(Clone)]
struct Loaded {
    track: Arc<TrackConfig>,
    bundle: Option<Arc<TrackBundle>>,
}

impl TrackContent {
    pub fn new(road_contact: RoadContactMode) -> Self {
        Self {
            road_contact,
            tracks: RwLock::new(HashMap::new()),
        }
    }

    /// Register the file a catalog track was read from. A track registered
    /// without one (built in memory) has no sidecars to load. Registering
    /// one again (a re-upload) drops what was loaded: the next session
    /// reads the new files, sessions already running keep theirs.
    pub fn register(&self, id: TrackConfigId, path: PathBuf) {
        self.write().insert(
            id,
            Arc::new(Slot {
                path,
                complete: OnceLock::new(),
            }),
        );
    }

    fn slot(&self, id: TrackConfigId) -> Option<Arc<Slot>> {
        self.tracks
            .read()
            .unwrap_or_else(|e| e.into_inner())
            .get(&id)
            .cloned()
    }

    fn write(&self) -> std::sync::RwLockWriteGuard<'_, HashMap<TrackConfigId, Arc<Slot>>> {
        self.tracks.write().unwrap_or_else(|e| e.into_inner())
    }

    /// The file a catalog track was read from.
    pub fn path_of(&self, id: TrackConfigId) -> Option<PathBuf> {
        self.slot(id).map(|slot| slot.path.clone())
    }

    /// Whether a session on this track would have to wait for a disk read.
    pub fn needs_loading(&self, id: TrackConfigId) -> bool {
        self.slot(id)
            .is_some_and(|slot| slot.complete.get().is_none())
    }

    /// The catalog track with its sidecars, loaded on the first call and
    /// shared after it. Blocking: a caller on the game loop goes through
    /// `game_loop::track_loads` first, which leaves this a lookup. Two
    /// callers asking at once load once, the second waiting for the first.
    pub fn complete(&self, catalog: &TrackConfig) -> Arc<TrackConfig> {
        let Some(slot) = self.slot(catalog.id) else {
            return Arc::new(catalog.clone());
        };
        self.loaded(&slot, catalog).track
    }

    /// The bundle of the files a loaded track was read from (the YAML and
    /// the sidecars the car step reads), for a predicting client; none for
    /// a track not loaded yet, one built in memory, or one whose YAML
    /// changed on disk since the catalog read it.
    pub fn bundle(&self, id: TrackConfigId) -> Option<Arc<TrackBundle>> {
        self.slot(id)?.complete.get()?.bundle.clone()
    }

    /// The loaded bundle with this checksum, whichever track it is.
    pub fn bundle_by_crc(&self, crc: u32) -> Option<Arc<TrackBundle>> {
        let slots: Vec<Arc<Slot>> = self
            .tracks
            .read()
            .unwrap_or_else(|e| e.into_inner())
            .values()
            .cloned()
            .collect();
        slots
            .iter()
            .filter_map(|slot| slot.complete.get()?.bundle.clone())
            .find(|bundle| bundle.crc == crc)
    }

    fn loaded(&self, slot: &Slot, catalog: &TrackConfig) -> Loaded {
        slot.complete
            .get_or_init(|| {
                let started = std::time::Instant::now();
                let mut track = catalog.clone();
                // A sidecar reader that panics leaves the track as the
                // catalog has it, driving on the centerline, rather than
                // taking the game loop down with it.
                let loaded = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                    // The files are read once: the sidecars the track drives
                    // on are the bytes its bundle hands out.
                    let files = TrackFiles::read(&slot.path, self.road_contact).ok();
                    let empty = TrackFiles::default();
                    TrackLoader::load_sidecars_from(
                        &mut track,
                        files.as_ref().unwrap_or(&empty),
                        &slot.path,
                        self.road_contact,
                    );
                    let bundle = files.and_then(|files| {
                        if content_crc(&files.yaml) != catalog.content_crc {
                            warn!(
                                "{} changed on disk since the server started; its physics bundle is not served",
                                slot.path.display()
                            );
                            return None;
                        }
                        Some(Arc::new(TrackBundle::of(&files)))
                    });
                    (track, bundle)
                }));
                let (track, bundle) = match loaded {
                    Ok(loaded) => loaded,
                    Err(_) => {
                        warn!(
                            "Loading the sidecars of {} ({}) panicked; it drives without them",
                            catalog.name,
                            slot.path.display()
                        );
                        (catalog.clone(), None)
                    }
                };
                info!(
                    "Loaded track content for {} in {} ms{}",
                    track.name,
                    started.elapsed().as_millis(),
                    bundle.as_ref().map_or(String::new(), |b| format!(
                        "; physics bundle {:08X}: {:.2} MB, {:.2} MB deflated",
                        b.crc,
                        b.raw_len as f64 / 1e6,
                        b.bytes.len() as f64 / 1e6
                    ))
                );
                Loaded {
                    track: Arc::new(track),
                    bundle,
                }
            })
            .clone()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn write_track(dir: &std::path::Path) -> PathBuf {
        let path = dir.join("Square.yaml");
        std::fs::write(
            &path,
            "name: Square\ntrack_id: 6f1c2c4e-3b1f-4c8a-9d57-1a2b3c4d5e6f\nnodes:\n  \
             - { x: 0.0, y: 0.0 }\n  - { x: 100.0, y: 0.0 }\n  - { x: 100.0, y: 100.0 }\n  \
             - { x: 0.0, y: 100.0 }\ndefault_width: 10.0\nclosed_loop: true\n",
        )
        .unwrap();
        path
    }

    /// The catalog entry carries no sidecars; the first `complete` loads
    /// them and every later one hands back the same track.
    #[test]
    fn a_track_is_completed_once_and_shared() {
        let dir = tempfile::tempdir().unwrap();
        let path = write_track(dir.path());
        let catalog = TrackLoader::load_catalog_entry(&path).unwrap();

        let content = TrackContent::new(RoadContactMode::Mesh);
        content.register(catalog.id, path);
        assert!(content.needs_loading(catalog.id));
        let first = content.complete(&catalog);
        assert!(!content.needs_loading(catalog.id));
        assert!(Arc::ptr_eq(&first, &content.complete(&catalog)));
        assert_eq!(first.centerline.len(), catalog.centerline.len());
    }

    /// Completing a catalog entry gives the track a full load gives: the
    /// same sidecars and the grid seated on the same road. Skipped until
    /// Monza's sidecars are baked (they are generated, not checked in).
    #[test]
    fn a_completed_track_is_the_track_a_full_load_gives() {
        let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../../apexsim-content/tracks/default/Monzarella/Monzarella.yaml");
        if !crate::walls::Walls::sidecar_path(&path).exists() {
            eprintln!("Monza's sidecars are not baked; skipping");
            return;
        }
        let full = TrackLoader::load_from_file_with(&path, RoadContactMode::Mesh).unwrap();
        let catalog = TrackLoader::load_catalog_entry(&path).unwrap();
        assert!(catalog.walls.is_none() && catalog.road_mesh.is_none());

        let content = TrackContent::new(RoadContactMode::Mesh);
        content.register(catalog.id, path);
        let complete = content.complete(&catalog);
        assert_eq!(complete.id, full.id);
        assert_eq!(complete.content_crc, full.content_crc);
        assert_eq!(complete.ground.is_some(), full.ground.is_some());
        assert_eq!(complete.curbs.is_some(), full.curbs.is_some());
        assert_eq!(complete.pit_lane.is_some(), full.pit_lane.is_some());
        assert_eq!(
            complete.walls.as_ref().map(|w| w.len()),
            full.walls.as_ref().map(|w| w.len())
        );
        assert_eq!(
            complete.road_mesh.as_ref().map(|m| m.triangle_count()),
            full.road_mesh.as_ref().map(|m| m.triangle_count())
        );
        let seats =
            |t: &TrackConfig| -> Vec<f32> { t.start_positions.iter().map(|s| s.z).collect() };
        assert_eq!(seats(&complete), seats(&full));
    }

    /// A loaded track's bundle holds the files it was read from, under their
    /// checksum; none before the load, and none once the YAML on disk is no
    /// longer the one the catalog read.
    #[test]
    fn a_loaded_track_is_bundled_from_the_files_it_read() {
        let dir = tempfile::tempdir().unwrap();
        let path = write_track(dir.path());
        std::fs::write(dir.path().join("Square.walls.msgpack"), b"not walls").unwrap();
        let catalog = TrackLoader::load_catalog_entry(&path).unwrap();
        let content = TrackContent::new(RoadContactMode::Mesh);
        content.register(catalog.id, path.clone());
        assert!(content.bundle(catalog.id).is_none(), "not loaded yet");
        content.complete(&catalog);
        let bundle = content.bundle(catalog.id).expect("a bundle");
        let files = TrackFiles::read(&path, RoadContactMode::Mesh).unwrap();
        assert_eq!(bundle.crc, files.checksum());
        assert_eq!(
            TrackFiles::decode(&bundle.bytes, Some(bundle.crc)).unwrap(),
            files
        );
        assert!(Arc::ptr_eq(
            &bundle,
            &content.bundle_by_crc(bundle.crc).unwrap()
        ));
        assert!(content.bundle_by_crc(bundle.crc ^ 1).is_none());

        // Edited since the catalog read it: driven, not handed out.
        let edited = TrackContent::new(RoadContactMode::Mesh);
        edited.register(catalog.id, path.clone());
        std::fs::write(&path, b"name: Square\ndefault_width: 11.0\n").unwrap();
        edited.complete(&catalog);
        assert!(edited.bundle(catalog.id).is_none());
    }

    /// A track nobody registered (built in memory) is complete as it is.
    #[test]
    fn an_unregistered_track_is_complete_as_it_is() {
        let dir = tempfile::tempdir().unwrap();
        let catalog = TrackLoader::load_catalog_entry(write_track(dir.path())).unwrap();
        let content = TrackContent::new(RoadContactMode::Mesh);
        assert!(!content.needs_loading(catalog.id));
        assert_eq!(content.complete(&catalog).id, catalog.id);
    }
}
