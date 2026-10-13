//! The server's own replays: a live session written to disk as a spectator
//! stream (`.apxs`, docs/game/spectator.md), the format the client plays
//! its own replays in and the showcases are rendered in.
//!
//! A recording is fed the records of a [`crate::session_stream::SessionStream`]
//! each tick. Records collect into blocks of a second of ticks, and a writer
//! thread compresses each block into a temporary file beside the replays as
//! it fills, so a race of any length costs the game loop a queue and nothing
//! more. Stopping writes the file's preamble (the header knows its last tick
//! only now), copies the blocks after it and closes with the index and the
//! trailer. A small `.meta.json` beside it holds the classification, so
//! listing the history reads no replay.

use std::collections::HashMap;
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::mpsc::{sync_channel, Receiver, SyncSender, TrySendError};
use std::sync::Mutex;
use std::thread::JoinHandle;

use tracing::{debug, warn};

use crate::data::*;
use crate::replay::{ReplayMetadata, ReplayParticipant};
use crate::session_stream::{SessionStream, StreamRecord};
use crate::spectator::{
    framed, Block, Index, IndexEntry, StreamEvent, StreamHeader, StreamPath, StreamRoster,
    FILE_MAGIC, FILE_VERSION,
};

/// Blocks waiting for the writer before the game loop starts dropping them
/// rather than wait: minutes of racing.
const QUEUE_BLOCKS: usize = 512;

/// A second of records, on its way to the writer.
struct Job {
    first_tick: u32,
    last_tick: u32,
    records: u32,
    raw: Vec<u8>,
}

/// Where a block landed in the temporary file, offsets from its start.
type Written = Vec<IndexEntry>;

/// One session being recorded.
pub struct Recording {
    session_id: SessionId,
    header: StreamHeader,
    roster: StreamRoster,
    path: StreamPath,
    preamble: Vec<StreamEvent>,
    meta: ReplayMetadata,
    block_ticks: u32,
    pending: Vec<u8>,
    pending_first: u32,
    pending_records: u32,
    first_tick: Option<u32>,
    last_tick: u32,
    sender: Option<SyncSender<Job>>,
    writer: Option<JoinHandle<std::io::Result<Written>>>,
    temp_path: PathBuf,
    /// Blocks dropped because the writer had fallen a whole queue behind.
    dropped: u64,
}

impl Recording {
    fn add(&mut self, record: &StreamRecord) {
        if self.pending_records > 0 && record.tick >= self.pending_first + self.block_ticks {
            self.flush();
        }
        if self.pending_records == 0 {
            self.pending_first = record.tick;
        }
        self.first_tick.get_or_insert(record.tick);
        self.last_tick = self.last_tick.max(record.tick);
        self.pending
            .extend_from_slice(&(record.body.len() as u32).to_be_bytes());
        self.pending.extend_from_slice(&record.body);
        self.pending_records += 1;
    }

    fn flush(&mut self) {
        if self.pending_records == 0 {
            return;
        }
        let job = Job {
            first_tick: self.pending_first,
            last_tick: self.last_tick,
            records: self.pending_records,
            raw: std::mem::take(&mut self.pending),
        };
        self.pending_records = 0;
        let Some(sender) = &self.sender else {
            return;
        };
        match sender.try_send(job) {
            Ok(()) => {}
            Err(TrySendError::Full(_)) => {
                if self.dropped == 0 {
                    warn!(
                        "Replay writer for session {} is behind; dropping blocks",
                        self.session_id
                    );
                }
                self.dropped += 1;
            }
            // The writer failed; finishing reports it.
            Err(TrySendError::Disconnected(_)) => self.sender = None,
        }
    }

    /// Stop the writer and wait for it.
    fn close_writer(&mut self) -> std::io::Result<Written> {
        self.flush();
        self.sender = None;
        match self.writer.take().map(|w| w.join()) {
            Some(Ok(result)) => result,
            Some(Err(_)) => Err(std::io::Error::other("the replay writer panicked")),
            None => Err(std::io::Error::other("the replay writer is gone")),
        }
    }

    /// Throw the recording away: stop the writer and delete its blocks.
    fn discard(mut self) {
        let _ = self.close_writer();
        let _ = std::fs::remove_file(&self.temp_path);
    }

    /// Write the finished replay into `dir` and return its path. `results`
    /// is the classification, `race_start_tick` the lights going out, and
    /// `game_mode` the mode the session ended in.
    pub fn finish(
        mut self,
        dir: &Path,
        results: Vec<ReplayParticipant>,
        race_start_tick: Option<u32>,
        game_mode: GameMode,
    ) -> std::io::Result<PathBuf> {
        let written = self.close_writer();
        let result = (|| {
            let written = written?;
            let first_tick = self
                .first_tick
                .ok_or_else(|| std::io::Error::other("nothing was recorded"))?;
            let mut header = self.header.clone();
            header.start_tick = first_tick;
            header.end_tick = self.last_tick;
            header.race_start_tick = race_start_tick;
            header.game_mode = game_mode;

            let mut preamble = Vec::new();
            preamble.extend_from_slice(FILE_MAGIC);
            preamble.extend_from_slice(&FILE_VERSION.to_be_bytes());
            preamble.extend_from_slice(&framed(&header.encode()));
            preamble.extend_from_slice(&framed(&self.roster.encode()));
            preamble.extend_from_slice(&framed(&self.path.encode()));
            for event in &self.preamble {
                preamble.extend_from_slice(&framed(&event.encode()));
            }

            let unix = std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map_or(0, |d| d.as_secs());
            let stem = format!("replay_{}_{}", self.session_id, unix);
            let replay_path = dir.join(format!("{stem}.apxs"));
            let mut out = std::io::BufWriter::new(std::fs::File::create(&replay_path)?);
            out.write_all(&preamble)?;
            let blocks_start = preamble.len() as u64;
            let mut temp = std::fs::File::open(&self.temp_path)?;
            let mut copied = 0u64;
            let mut buf = vec![0u8; 1 << 16];
            loop {
                let n = temp.read(&mut buf)?;
                if n == 0 {
                    break;
                }
                out.write_all(&buf[..n])?;
                copied += n as u64;
            }
            let index = Index {
                blocks: written
                    .iter()
                    .map(|b| IndexEntry {
                        first_tick: b.first_tick,
                        offset: blocks_start + b.offset,
                        records: b.records,
                    })
                    .collect(),
            };
            let index_offset = blocks_start + copied;
            out.write_all(&framed(&index.encode()))?;
            out.write_all(&index_offset.to_be_bytes())?;
            out.flush()?;

            let mut meta = self.meta.clone();
            meta.duration_ticks = self.last_tick.saturating_sub(first_tick);
            meta.race_start_tick = race_start_tick;
            meta.participants = results;
            let json = serde_json::to_vec(&meta).map_err(std::io::Error::other)?;
            std::fs::write(dir.join(format!("{stem}.meta.json")), json)?;
            Ok(replay_path)
        })();
        let _ = std::fs::remove_file(&self.temp_path);
        if self.dropped > 0 {
            warn!(
                "Replay for session {} is missing {} block(s): the disk could not keep up",
                self.session_id, self.dropped
            );
        }
        result
    }
}

/// The writer thread: every block the channel brings, compressed and
/// appended to the file at `path`, until the recording hangs up.
fn write_blocks(path: &Path, jobs: Receiver<Job>) -> std::io::Result<Written> {
    let mut file = std::io::BufWriter::new(std::fs::File::create(path)?);
    let mut written = Written::new();
    let mut offset = 0u64;
    for job in jobs {
        let mut encoder = flate2::write::ZlibEncoder::new(Vec::new(), flate2::Compression::new(6));
        encoder.write_all(&job.raw)?;
        let block = Block {
            first_tick: job.first_tick,
            last_tick: job.last_tick,
            records: job.records,
            raw_len: job.raw.len() as u32,
            zlib: encoder.finish()?,
        };
        let bytes = framed(&block.encode());
        written.push(IndexEntry {
            first_tick: job.first_tick,
            offset,
            records: job.records,
        });
        file.write_all(&bytes)?;
        offset += bytes.len() as u64;
    }
    file.flush()?;
    Ok(written)
}

/// Every session being recorded, behind one lock the game loop takes for
/// the length of a push.
pub struct StreamRecorders {
    dir: PathBuf,
    active: Mutex<HashMap<SessionId, Recording>>,
}

impl StreamRecorders {
    pub fn new(dir: PathBuf) -> Self {
        Self {
            dir,
            active: Mutex::new(HashMap::new()),
        }
    }

    /// The folder the replays are written to.
    pub fn dir(&self) -> &Path {
        &self.dir
    }

    fn lock(&self) -> std::sync::MutexGuard<'_, HashMap<SessionId, Recording>> {
        self.active.lock().unwrap_or_else(|e| e.into_inner())
    }

    /// Begin recording a session whose stream is `stream`. A recording
    /// already running for it (a session that went back from racing without
    /// finishing) is dropped.
    pub fn start(
        &self,
        session: &crate::game_session::GameSession,
        stream: &SessionStream,
        meta: ReplayMetadata,
    ) {
        let stem = meta.track_stem.clone().unwrap_or_default();
        let session_id = meta.session_id;
        let temp_path = self.dir.join(format!(".recording_{session_id}.blocks"));
        let (sender, receiver) = sync_channel(QUEUE_BLOCKS);
        let writer = {
            let dir = self.dir.clone();
            let path = temp_path.clone();
            std::thread::Builder::new()
                .name("replay-writer".into())
                .spawn(move || {
                    std::fs::create_dir_all(&dir)?;
                    write_blocks(&path, receiver)
                })
        };
        let writer = match writer {
            Ok(handle) => handle,
            Err(e) => {
                warn!("Cannot record a replay for session {}: {}", session_id, e);
                return;
            }
        };
        let recording = Recording {
            session_id,
            header: stream.header(session, &stem),
            roster: stream.roster().clone(),
            path: stream.path(session),
            preamble: vec![stream.encoder.sectors(session)],
            meta,
            block_ticks: u32::from(session.tick_rate_hz()).max(1),
            pending: Vec::new(),
            pending_first: 0,
            pending_records: 0,
            first_tick: None,
            last_tick: 0,
            sender: Some(sender),
            writer: Some(writer),
            temp_path,
            dropped: 0,
        };
        let replaced = self.lock().insert(session_id, recording);
        if let Some(old) = replaced {
            std::thread::spawn(move || old.discard());
        }
        debug!("Started recording replay for session {}", session_id);
    }

    pub fn is_recording(&self, session_id: &SessionId) -> bool {
        self.lock().contains_key(session_id)
    }

    /// Add one tick's records to a session's recording; nothing if it has
    /// none.
    pub fn record(&self, session_id: &SessionId, records: &[StreamRecord]) {
        if let Some(recording) = self.lock().get_mut(session_id) {
            for record in records {
                recording.add(record);
            }
        }
    }

    /// Take a recording out to finish it ([`Recording::finish`]).
    pub fn take(&self, session_id: &SessionId) -> Option<Recording> {
        self.lock().remove(session_id)
    }

    /// Drop the recordings of sessions that are gone without finishing
    /// (everyone left mid-race), deleting what they had written.
    pub fn discard_except(&self, live: impl Fn(&SessionId) -> bool) {
        let gone: Vec<Recording> = {
            let mut active = self.lock();
            let ids: Vec<SessionId> = active.keys().filter(|id| !live(id)).copied().collect();
            ids.iter().filter_map(|id| active.remove(id)).collect()
        };
        for recording in gone {
            debug!(
                "Discarding the replay of ended session {}",
                recording.session_id
            );
            std::thread::spawn(move || recording.discard());
        }
    }

    /// Sessions being recorded.
    pub fn recording_count(&self) -> usize {
        self.lock().len()
    }
}

/// What the history lists of one replay: its sidecar, else (a replay from
/// before streams) its `.bin` header.
pub fn read_listing(path: &Path) -> Option<ReplayMetadata> {
    let name = path.file_name()?.to_string_lossy().into_owned();
    if name.ends_with(".meta.json") {
        let bytes = std::fs::read(path).ok()?;
        return serde_json::from_slice(&bytes).ok();
    }
    if name.ends_with(".bin") {
        return crate::replay::read_replay_header(path)
            .ok()
            .map(|h| h.metadata);
    }
    None
}

/// Whether a file in the replay folder is one the history lists.
pub fn is_listed_file(name: &str) -> bool {
    name.starts_with("replay_") && (name.ends_with(".meta.json") || name.ends_with(".bin"))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::spectator::{Record, StreamFile, RECORD_FRAME};
    use tempfile::TempDir;

    /// Records a few seconds of an AI race into the folder and returns the
    /// replay's path.
    fn record_a_race(dir: &Path) -> (PathBuf, Vec<(u32, Vec<u8>)>) {
        use crate::replay_tools::{start_race, SimulateOptions};
        let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("..");
        let opts = SimulateOptions {
            track_path: root.join("content/tracks/default/Zandvoort/Zandvoort.yaml"),
            cars_dir: root.join("content/cars/default"),
            ai_count: 3,
            laps: 1,
            countdown_seconds: 1,
            record_hz: 30,
            seed: Some(3),
            tick_rate: 240,
            ..SimulateOptions::default()
        };
        let mut race = start_race(&opts).expect("race").race;
        let names = HashMap::new();
        let mut stream = SessionStream::new(&race, 30, &names);
        let recorders = StreamRecorders::new(dir.to_path_buf());
        let sid = race.session.id;
        let meta = ReplayMetadata {
            session_id: sid,
            track_config_id: race.track_config.id,
            track_name: "Zandervoort".into(),
            recorded_at: 1,
            duration_ticks: 0,
            tick_rate: 240,
            participants: vec![],
            conditions: race.session.conditions,
            race_start_tick: None,
            track_stem: Some("Zandvoort".into()),
            track_length_m: race.track_length_m(),
        };
        recorders.start(&race, &stream, meta);
        assert!(recorders.is_recording(&sid));
        let mut kept = Vec::new();
        for _ in 0..240 * 6 {
            let inputs: HashMap<PlayerId, PlayerInputData> = race
                .session
                .participants
                .keys()
                .map(|id| (*id, race.generate_ai_input(id)))
                .collect();
            race.tick(&inputs);
            let laps = race.take_lap_events();
            let pits = race.take_pit_events();
            let out = stream.tick(&race, &laps, &pits, &names, false);
            for r in &out.records {
                kept.push((r.tick, r.body.clone()));
            }
            recorders.record(&sid, &out.records);
        }
        let recording = recorders.take(&sid).expect("recording");
        assert_eq!(recorders.recording_count(), 0);
        let results = vec![ReplayParticipant {
            player_id: PlayerId::nil(),
            player_name: "AI 1".into(),
            car_config_id: CarConfigId::nil(),
            finish_position: Some(1),
            is_ai: true,
            livery: 0,
            best_lap_ms: None,
            laps: 0,
        }];
        let path = recording
            .finish(
                dir,
                results,
                race.session.race_start_tick,
                race.session.game_mode,
            )
            .expect("replay written");
        (path, kept)
    }

    #[test]
    fn a_session_is_recorded_as_a_stream_file() {
        let dir = TempDir::new().unwrap();
        let (path, kept) = record_a_race(dir.path());

        // Only the replay and its sidecar are left: the blocks' temporary
        // file is gone.
        let mut names: Vec<String> = std::fs::read_dir(dir.path())
            .unwrap()
            .map(|e| e.unwrap().file_name().to_string_lossy().into_owned())
            .collect();
        names.sort();
        assert_eq!(names.len(), 2, "{names:?}");
        assert!(names.iter().any(|n| n.ends_with(".apxs")));
        assert!(names.iter().any(|n| n.ends_with(".meta.json")));

        // It opens as any stream does, and every record written is in it,
        // in order.
        let file = StreamFile::open(&path).unwrap();
        assert_eq!(file.header.track.stem, "Zandvoort");
        assert_eq!(file.header.start_tick, kept[0].0);
        assert_eq!(file.header.end_tick, kept.last().unwrap().0);
        assert_eq!(file.roster.entries.len(), 3);
        assert!(file.path.as_ref().is_some_and(|p| p.points.len() > 300));
        assert!(file.block_count() >= 5, "{}", file.block_count());
        let records = file.records().unwrap();
        assert_eq!(records.len(), kept.len());
        assert!(records.iter().zip(&kept).all(|(r, (_, k))| r == k));
        let frames = records
            .iter()
            .filter(|b| crate::spectator::record_type(b) == Some(RECORD_FRAME))
            .count();
        assert!(frames > 100, "{frames}");
        assert!(records
            .iter()
            .any(|b| matches!(Record::decode(b), Ok(Record::State(_)))));
        // A seek lands on a block that holds the tick.
        let mid = (kept[0].0 + kept.last().unwrap().0) / 2;
        let block = file.block(file.block_for_tick(mid)).unwrap();
        assert!(crate::spectator::record_tick(&block[0]).is_some_and(|t| t <= mid));

        // The sidecar is what the history lists.
        let meta_path = path.with_extension("meta.json");
        let meta = read_listing(&meta_path).expect("sidecar reads");
        assert_eq!(meta.track_name, "Zandervoort");
        assert_eq!(meta.participants[0].finish_position, Some(1));
        assert!(meta.duration_ticks > 240 * 5);
        assert!(is_listed_file(
            meta_path.file_name().unwrap().to_str().unwrap()
        ));
    }

    #[test]
    fn an_abandoned_recording_is_discarded() {
        let dir = TempDir::new().unwrap();
        let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("..");
        let opts = crate::replay_tools::SimulateOptions {
            track_path: root.join("content/tracks/default/Zandvoort/Zandvoort.yaml"),
            cars_dir: root.join("content/cars/default"),
            ai_count: 1,
            seed: Some(1),
            ..crate::replay_tools::SimulateOptions::default()
        };
        let race = crate::replay_tools::start_race(&opts).expect("race").race;
        let names = HashMap::new();
        let stream = SessionStream::new(&race, 30, &names);
        let recorders = StreamRecorders::new(dir.path().to_path_buf());
        let sid = race.session.id;
        let meta = ReplayMetadata {
            session_id: sid,
            track_config_id: race.track_config.id,
            track_name: "x".into(),
            recorded_at: 0,
            duration_ticks: 0,
            tick_rate: 240,
            participants: vec![],
            conditions: race.session.conditions,
            race_start_tick: None,
            track_stem: Some("Zandvoort".into()),
            track_length_m: 0.0,
        };
        recorders.start(&race, &stream, meta);
        recorders.record(
            &sid,
            &[StreamRecord {
                tick: 1,
                kind: RECORD_FRAME,
                body: vec![0x90],
            }],
        );
        recorders.discard_except(|_| false);
        assert_eq!(recorders.recording_count(), 0);
        // The writer is stopped and its file deleted off the loop.
        for _ in 0..300 {
            if std::fs::read_dir(dir.path()).unwrap().next().is_none() {
                return;
            }
            std::thread::sleep(std::time::Duration::from_millis(10));
        }
        panic!("the recording's blocks were not deleted");
    }
}
