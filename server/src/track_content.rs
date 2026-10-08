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
use crate::data::{TrackConfig, TrackConfigId};
use crate::track_loader::TrackLoader;
use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::{Arc, OnceLock};
use tracing::{info, warn};

pub struct TrackContent {
    road_contact: RoadContactMode,
    tracks: HashMap<TrackConfigId, Slot>,
}

struct Slot {
    /// The track file, where its sidecars sit beside it.
    path: PathBuf,
    complete: OnceLock<Arc<TrackConfig>>,
}

impl TrackContent {
    pub fn new(road_contact: RoadContactMode) -> Self {
        Self {
            road_contact,
            tracks: HashMap::new(),
        }
    }

    /// Register the file a catalog track was read from. A track registered
    /// without one (built in memory) has no sidecars to load.
    pub fn register(&mut self, id: TrackConfigId, path: PathBuf) {
        self.tracks.insert(
            id,
            Slot {
                path,
                complete: OnceLock::new(),
            },
        );
    }

    /// The file a catalog track was read from.
    pub fn path_of(&self, id: TrackConfigId) -> Option<&std::path::Path> {
        self.tracks.get(&id).map(|slot| slot.path.as_path())
    }

    /// Whether a session on this track would have to wait for a disk read.
    pub fn needs_loading(&self, id: TrackConfigId) -> bool {
        self.tracks
            .get(&id)
            .is_some_and(|slot| slot.complete.get().is_none())
    }

    /// The catalog track with its sidecars, loaded on the first call and
    /// shared after it. Blocking: a caller on the game loop goes through
    /// `game_loop::track_loads` first, which leaves this a lookup. Two
    /// callers asking at once load once, the second waiting for the first.
    pub fn complete(&self, catalog: &TrackConfig) -> Arc<TrackConfig> {
        let Some(slot) = self.tracks.get(&catalog.id) else {
            return Arc::new(catalog.clone());
        };
        slot.complete
            .get_or_init(|| {
                let started = std::time::Instant::now();
                let mut track = catalog.clone();
                // A sidecar reader that panics leaves the track as the
                // catalog has it, driving on the centerline, rather than
                // taking the game loop down with it.
                let loaded = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                    TrackLoader::load_sidecars(&mut track, &slot.path, self.road_contact);
                    track
                }));
                let track = match loaded {
                    Ok(track) => track,
                    Err(_) => {
                        warn!(
                            "Loading the sidecars of {} ({}) panicked; it drives without them",
                            catalog.name,
                            slot.path.display()
                        );
                        catalog.clone()
                    }
                };
                info!(
                    "Loaded track content for {} in {} ms",
                    track.name,
                    started.elapsed().as_millis()
                );
                Arc::new(track)
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

        let mut content = TrackContent::new(RoadContactMode::Mesh);
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
            .join("../content/tracks/default/Monza/Monza.yaml");
        if !crate::walls::Walls::sidecar_path(&path).exists() {
            eprintln!("Monza's sidecars are not baked; skipping");
            return;
        }
        let full = TrackLoader::load_from_file_with(&path, RoadContactMode::Mesh).unwrap();
        let catalog = TrackLoader::load_catalog_entry(&path).unwrap();
        assert!(catalog.walls.is_none() && catalog.road_mesh.is_none());

        let mut content = TrackContent::new(RoadContactMode::Mesh);
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
