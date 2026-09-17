//! Lap records: the best legal lap each driver has ever set on a track in a
//! car, kept across sessions and across restarts.
//!
//! Nothing in the sim reads this. The store is written when a lap beats a
//! record and read when a driver joins a session, so the HUD can show the
//! delta to their own best rather than only to their best *today*.
//!
//! **Identity.** A record is keyed by the driver's *name*, not their player
//! id: the id is minted per connection, so a UUID key would forget every
//! record the moment a player reconnected. AI drivers are never recorded.
//!
//! **The ghost.** A record carries an optional trace of the lap it was set
//! on — the car's pose a few dozen times a second, from the line to the line
//! ([`GhostLap`]). Nothing plays it back yet; it exists so the ghost car and
//! time trial can be built on records that already have their laps stored.
//! Traces live in their own files under `ghosts/`, because a record is a few
//! hundred bytes and a trace is tens of kilobytes.

use crate::data::{CarConfigId, TrackConfigId};
use crate::laps::SECTOR_COUNT;
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::sync::RwLock;
use tracing::{debug, warn};

/// How often a ghost lap is sampled. 20 Hz is about 3 m at racing speed, and
/// the client interpolates between samples exactly as it does for a live car.
pub const GHOST_SAMPLE_HZ: f32 = 20.0;

/// One moment of a recorded lap. Everything the client needs to drive a
/// puppet car through it; the same fields a telemetry frame carries.
#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
pub struct GhostSample {
    /// Milliseconds since the lap started.
    pub t_ms: u32,
    pub x: f32,
    pub y: f32,
    pub z: f32,
    pub yaw_rad: f32,
    pub pitch_rad: f32,
    pub roll_rad: f32,
    pub speed_mps: f32,
    pub steering: f32,
    pub throttle: f32,
    pub brake: f32,
    pub gear: i8,
    pub engine_rpm: f32,
}

/// A recorded lap, start line to start line.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct GhostLap {
    pub lap_time_ms: u32,
    pub sample_hz: f32,
    pub samples: Vec<GhostSample>,
}

/// The best legal lap one driver has set with one car on one track.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct LapRecord {
    pub player: String,
    pub track_id: TrackConfigId,
    pub car_config_id: CarConfigId,
    pub lap_time_ms: u32,
    pub splits_ms: [u32; SECTOR_COUNT],
    /// When it was set, RFC 3339. Display only.
    pub recorded_at: String,
    /// File under `ghosts/` holding the lap's trace, when one was captured.
    #[serde(default)]
    pub ghost_file: Option<String>,
}

type RecordKey = (String, TrackConfigId, CarConfigId);

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
struct RecordsFile {
    version: u32,
    records: Vec<LapRecord>,
}

/// The records on disk, held in memory and rewritten when one is beaten.
///
/// Every method is cheap enough to call from the game loop *between* ticks
/// but not during one: `submit` writes files. The loop calls it from the
/// broadcast phase, after the state lock is dropped.
#[derive(Debug)]
pub struct RecordStore {
    dir: Option<PathBuf>,
    records: RwLock<BTreeMap<RecordKey, LapRecord>>,
}

impl RecordStore {
    /// A store that keeps records in memory only — used when records are
    /// switched off, and by tests.
    pub fn in_memory() -> Self {
        Self {
            dir: None,
            records: RwLock::new(BTreeMap::new()),
        }
    }

    /// Open (or create) the store in `dir`. A file that cannot be read is
    /// logged and treated as empty: a corrupt records file must never stop
    /// the server from racing.
    pub fn open(dir: impl AsRef<Path>) -> Self {
        let dir = dir.as_ref().to_path_buf();
        if let Err(e) = std::fs::create_dir_all(dir.join("ghosts")) {
            warn!("Lap records disabled: cannot create {}: {e}", dir.display());
            return Self::in_memory();
        }
        let mut records = BTreeMap::new();
        let path = records_path(&dir);
        match std::fs::read(&path) {
            Ok(bytes) => match serde_json::from_slice::<RecordsFile>(&bytes) {
                Ok(file) => {
                    for record in file.records {
                        records.insert(key_of(&record), record);
                    }
                    debug!(
                        "Loaded {} lap records from {}",
                        records.len(),
                        path.display()
                    );
                }
                Err(e) => warn!("Ignoring unreadable lap records {}: {e}", path.display()),
            },
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
            Err(e) => warn!("Cannot read lap records {}: {e}", path.display()),
        }
        Self {
            dir: Some(dir),
            records: RwLock::new(records),
        }
    }

    /// This driver's record with this car on this track.
    pub fn best(
        &self,
        player: &str,
        track_id: TrackConfigId,
        car_config_id: CarConfigId,
    ) -> Option<LapRecord> {
        let key = (player.to_string(), track_id, car_config_id);
        self.records.read().ok()?.get(&key).cloned()
    }

    /// The fastest lap anyone has set with this car on this track — the
    /// track record the leaderboards will show.
    pub fn track_best(
        &self,
        track_id: TrackConfigId,
        car_config_id: CarConfigId,
    ) -> Option<LapRecord> {
        let records = self.records.read().ok()?;
        records
            .values()
            .filter(|r| r.track_id == track_id && r.car_config_id == car_config_id)
            .min_by_key(|r| (r.lap_time_ms, r.player.clone()))
            .cloned()
    }

    /// Offer a completed legal lap. Returns the new record when it beat the
    /// stored one (or when there was none), and nothing otherwise; the
    /// caller tells the driver either way.
    pub fn submit(
        &self,
        player: &str,
        track_id: TrackConfigId,
        car_config_id: CarConfigId,
        lap_time_ms: u32,
        splits_ms: [u32; SECTOR_COUNT],
        ghost: Option<GhostLap>,
    ) -> Option<LapRecord> {
        if player.is_empty() || lap_time_ms == 0 {
            return None;
        }
        let key = (player.to_string(), track_id, car_config_id);
        {
            let records = self.records.read().ok()?;
            if let Some(existing) = records.get(&key) {
                if existing.lap_time_ms <= lap_time_ms {
                    return None;
                }
            }
        }

        let previous_ghost = self
            .records
            .read()
            .ok()
            .and_then(|r| r.get(&key).and_then(|rec| rec.ghost_file.clone()));
        let ghost_file = ghost.and_then(|lap| self.write_ghost(&key, &lap));
        let record = LapRecord {
            player: player.to_string(),
            track_id,
            car_config_id,
            lap_time_ms,
            splits_ms,
            recorded_at: now_rfc3339(),
            ghost_file,
        };
        self.records.write().ok()?.insert(key, record.clone());
        self.save();
        // The lap this one beat is gone: so is its trace.
        if let (Some(dir), Some(old)) = (&self.dir, previous_ghost) {
            if Some(&old) != record.ghost_file.as_ref() {
                let _ = std::fs::remove_file(dir.join("ghosts").join(old));
            }
        }
        Some(record)
    }

    /// The trace of a recorded lap, when it has one.
    pub fn ghost(&self, record: &LapRecord) -> Option<GhostLap> {
        let dir = self.dir.as_ref()?;
        let file = record.ghost_file.as_ref()?;
        let bytes = std::fs::read(dir.join("ghosts").join(file)).ok()?;
        rmp_serde::from_slice(&bytes).ok()
    }

    /// Number of records held. Diagnostics and tests.
    pub fn len(&self) -> usize {
        self.records.read().map(|r| r.len()).unwrap_or(0)
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    fn write_ghost(&self, key: &RecordKey, lap: &GhostLap) -> Option<String> {
        let dir = self.dir.as_ref()?;
        if lap.samples.is_empty() {
            return None;
        }
        let name = ghost_file_name(key);
        let bytes = rmp_serde::to_vec_named(lap).ok()?;
        match std::fs::write(dir.join("ghosts").join(&name), bytes) {
            Ok(()) => Some(name),
            Err(e) => {
                warn!("Cannot write ghost lap {name}: {e}");
                None
            }
        }
    }

    /// Rewrite the records file. Whole-file: there are hundreds of records,
    /// not thousands, and a partial write is worse than a slow one.
    fn save(&self) {
        let Some(dir) = &self.dir else { return };
        let Ok(records) = self.records.read() else {
            return;
        };
        let file = RecordsFile {
            version: 1,
            records: records.values().cloned().collect(),
        };
        drop(records);
        let path = records_path(dir);
        let tmp = path.with_extension("json.tmp");
        match serde_json::to_vec_pretty(&file) {
            Ok(bytes) => {
                if let Err(e) = std::fs::write(&tmp, bytes) {
                    warn!("Cannot write lap records {}: {e}", tmp.display());
                    return;
                }
                if let Err(e) = std::fs::rename(&tmp, &path) {
                    warn!("Cannot replace lap records {}: {e}", path.display());
                }
            }
            Err(e) => warn!("Cannot encode lap records: {e}"),
        }
    }
}

impl Default for RecordStore {
    fn default() -> Self {
        Self::in_memory()
    }
}

fn records_path(dir: &Path) -> PathBuf {
    dir.join("lap_records.json")
}

fn key_of(record: &LapRecord) -> RecordKey {
    (record.player.clone(), record.track_id, record.car_config_id)
}

/// A stable file name per (driver, track, car): a record replaces its own
/// trace rather than filling the folder with every lap ever set. The player
/// name is hashed because it is free text and this is a path.
fn ghost_file_name(key: &RecordKey) -> String {
    use std::hash::{Hash, Hasher};
    let mut hasher = std::collections::hash_map::DefaultHasher::new();
    key.0.hash(&mut hasher);
    format!(
        "{:016x}-{}-{}.msgpack",
        hasher.finish(),
        key.1.simple(),
        key.2.simple()
    )
}

fn now_rfc3339() -> String {
    use std::time::{SystemTime, UNIX_EPOCH};
    let secs = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    // Civil date from a Unix timestamp (Howard Hinnant's days_from_civil,
    // inverted). No chrono in this crate, and a record's date is display
    // only.
    let days = (secs / 86_400) as i64;
    let tod = secs % 86_400;
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = if m <= 2 { y + 1 } else { y };
    format!(
        "{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z",
        y,
        m,
        d,
        tod / 3600,
        (tod % 3600) / 60,
        tod % 60
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use uuid::Uuid;

    fn ghost(time_ms: u32) -> GhostLap {
        GhostLap {
            lap_time_ms: time_ms,
            sample_hz: GHOST_SAMPLE_HZ,
            samples: vec![GhostSample {
                t_ms: 0,
                x: 1.0,
                y: 2.0,
                z: 3.0,
                yaw_rad: 0.5,
                pitch_rad: 0.0,
                roll_rad: 0.0,
                speed_mps: 60.0,
                steering: 0.1,
                throttle: 1.0,
                brake: 0.0,
                gear: 4,
                engine_rpm: 9000.0,
            }],
        }
    }

    #[test]
    fn a_faster_lap_takes_the_record_and_a_slower_one_does_not() {
        let store = RecordStore::in_memory();
        let (track, car) = (Uuid::new_v4(), Uuid::new_v4());
        assert!(store
            .submit("Ayrton", track, car, 82_500, [27_000, 28_000, 27_500], None)
            .is_some());
        assert!(store
            .submit("Ayrton", track, car, 83_000, [27_000, 28_000, 28_000], None)
            .is_none());
        assert!(store
            .submit("Ayrton", track, car, 81_900, [26_800, 27_900, 27_200], None)
            .is_some());
        assert_eq!(
            store.best("Ayrton", track, car).unwrap().lap_time_ms,
            81_900
        );
        assert_eq!(store.len(), 1, "one record per driver, track and car");
    }

    #[test]
    fn records_are_kept_per_car_and_track() {
        let store = RecordStore::in_memory();
        let (track_a, track_b) = (Uuid::new_v4(), Uuid::new_v4());
        let (car_a, car_b) = (Uuid::new_v4(), Uuid::new_v4());
        store.submit("Ayrton", track_a, car_a, 82_000, [0; 3], None);
        store.submit("Ayrton", track_a, car_b, 90_000, [0; 3], None);
        store.submit("Ayrton", track_b, car_a, 70_000, [0; 3], None);
        assert_eq!(store.len(), 3);
        assert_eq!(
            store.best("Ayrton", track_a, car_b).unwrap().lap_time_ms,
            90_000
        );
        assert!(store.best("Alain", track_a, car_a).is_none());
    }

    #[test]
    fn the_track_best_is_the_fastest_of_everyone() {
        let store = RecordStore::in_memory();
        let (track, car) = (Uuid::new_v4(), Uuid::new_v4());
        store.submit("Ayrton", track, car, 82_000, [0; 3], None);
        store.submit("Alain", track, car, 81_000, [0; 3], None);
        store.submit("Nelson", track, car, 83_000, [0; 3], None);
        assert_eq!(store.track_best(track, car).unwrap().player, "Alain");
    }

    #[test]
    fn records_and_their_ghosts_survive_a_restart() {
        let dir = std::env::temp_dir().join(format!("apexsim-records-{}", Uuid::new_v4()));
        let (track, car) = (Uuid::new_v4(), Uuid::new_v4());
        {
            let store = RecordStore::open(&dir);
            store.submit(
                "Ayrton",
                track,
                car,
                82_500,
                [27_000, 28_000, 27_500],
                Some(ghost(82_500)),
            );
        }
        let reopened = RecordStore::open(&dir);
        let record = reopened.best("Ayrton", track, car).expect("record kept");
        assert_eq!(record.lap_time_ms, 82_500);
        assert_eq!(record.splits_ms, [27_000, 28_000, 27_500]);
        let lap = reopened.ghost(&record).expect("ghost kept");
        assert_eq!(lap.samples.len(), 1);
        assert_eq!(lap.samples[0].gear, 4);
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_beaten_record_drops_its_old_trace() {
        let dir = std::env::temp_dir().join(format!("apexsim-records-{}", Uuid::new_v4()));
        let (track, car) = (Uuid::new_v4(), Uuid::new_v4());
        let store = RecordStore::open(&dir);
        store.submit("Ayrton", track, car, 82_500, [0; 3], Some(ghost(82_500)));
        store.submit("Ayrton", track, car, 81_000, [0; 3], Some(ghost(81_000)));
        let ghosts: Vec<_> = std::fs::read_dir(dir.join("ghosts"))
            .unwrap()
            .filter_map(|e| e.ok())
            .collect();
        assert_eq!(ghosts.len(), 1, "one trace per record");
        let record = store.best("Ayrton", track, car).unwrap();
        assert_eq!(store.ghost(&record).unwrap().lap_time_ms, 81_000);
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_corrupt_records_file_is_ignored_not_fatal() {
        let dir = std::env::temp_dir().join(format!("apexsim-records-{}", Uuid::new_v4()));
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(records_path(&dir), b"{ not json").unwrap();
        let store = RecordStore::open(&dir);
        assert!(store.is_empty());
        assert!(store
            .submit(
                "Ayrton",
                Uuid::new_v4(),
                Uuid::new_v4(),
                80_000,
                [0; 3],
                None
            )
            .is_some());
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn the_timestamp_is_a_readable_date() {
        let stamp = now_rfc3339();
        assert_eq!(stamp.len(), 20, "{stamp}");
        assert!(stamp.starts_with("20"), "{stamp}");
        assert!(stamp.ends_with('Z'), "{stamp}");
    }
}
