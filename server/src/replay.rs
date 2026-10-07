use crate::data::*;
use crate::network::Telemetry;
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::Arc;
use tokio::fs::{self, File};
use tokio::sync::RwLock;
use tracing::{debug, warn};

/// Replay metadata
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ReplayMetadata {
    pub session_id: SessionId,
    pub track_config_id: TrackConfigId,
    pub track_name: String,
    pub recorded_at: u64, // Unix timestamp
    pub duration_ticks: u32,
    pub tick_rate: u16,
    pub participants: Vec<ReplayParticipant>,
    /// The sky the session ran under, so a playback can draw the same one.
    /// A file from before the field is a sunny 13:00, which is what it was.
    #[serde(default)]
    pub conditions: SessionConditions,
    /// The tick the lights went out, when the recording holds a race start.
    #[serde(default)]
    pub race_start_tick: Option<u32>,
    /// The track file's stem (`Zandvoort` for `Zandvoort.yaml`): what the
    /// client's level and catalog are named by when an id lookup fails.
    #[serde(default)]
    pub track_stem: Option<String>,
    /// Lap length in metres, so a reader can wrap stations without the track.
    #[serde(default)]
    pub track_length_m: f32,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ReplayParticipant {
    pub player_id: PlayerId,
    pub player_name: String,
    pub car_config_id: CarConfigId,
    pub finish_position: Option<u8>,
    #[serde(default)]
    pub is_ai: bool,
    /// The livery worn, as the roster carries it (0 = as authored).
    #[serde(default)]
    pub livery: u8,
}

/// A single frame of replay data
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ReplayFrame {
    pub tick: u32,
    pub telemetry: Telemetry,
}

/// Replay file header
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ReplayHeader {
    pub version: u32,
    pub metadata: ReplayMetadata,
    pub frame_count: u32,
}

/// The file format: `[u32 le header length][header msgpack]` then, per
/// frame, `[u32 le frame length][frame msgpack]`, both named-field
/// MessagePack. Version 2 added the metadata's conditions, start tick and
/// track stem, all optional on the way in, so a version 1 file still reads.
/// Version 3 writes the frames as one zlib stream after the header, which
/// is what lets the server stream a recording to disk as it goes instead
/// of holding a whole race in memory; the header stays plain, so listing
/// replays reads no frames. Older files still read.
pub const REPLAY_FORMAT_VERSION: u32 = 3;
/// The first version whose frames are a zlib stream.
const ZLIB_FRAMES_VERSION: u32 = 3;

/// Frames waiting for the writer thread before the game loop starts
/// dropping them rather than wait: about half a minute at the telemetry
/// rate, a few megabytes for a full grid.
const RECORDING_QUEUE_FRAMES: usize = 2048;

fn invalid_data(e: impl std::error::Error + Send + Sync + 'static) -> std::io::Error {
    std::io::Error::new(std::io::ErrorKind::InvalidData, e)
}

/// One frame as the file holds it: `[u32 le length][named msgpack]`.
fn write_frame(writer: &mut impl std::io::Write, frame: &ReplayFrame) -> std::io::Result<()> {
    let frame_bytes = rmp_serde::to_vec_named(frame).map_err(invalid_data)?;
    writer.write_all(&(frame_bytes.len() as u32).to_le_bytes())?;
    writer.write_all(&frame_bytes)
}

/// The header as the file starts: `[u32 le length][named msgpack]`.
fn write_header(writer: &mut impl std::io::Write, header: &ReplayHeader) -> std::io::Result<()> {
    let header_bytes = rmp_serde::to_vec_named(header).map_err(invalid_data)?;
    writer.write_all(&(header_bytes.len() as u32).to_le_bytes())?;
    writer.write_all(&header_bytes)
}

fn read_frames(reader: &mut impl std::io::Read, count: u32) -> std::io::Result<Vec<ReplayFrame>> {
    let mut len = [0u8; 4];
    let mut frames = Vec::with_capacity(count as usize);
    for _ in 0..count {
        reader.read_exact(&mut len)?;
        let mut frame_bytes = vec![0u8; u32::from_le_bytes(len) as usize];
        reader.read_exact(&mut frame_bytes)?;
        frames.push(rmp_serde::from_slice(&frame_bytes).map_err(invalid_data)?);
    }
    Ok(frames)
}

/// Write a replay file synchronously (the offline tools; the server's own
/// recorder writes from the game loop through [`ReplayManager`]).
pub fn write_replay_file(
    path: &std::path::Path,
    metadata: ReplayMetadata,
    frames: &[ReplayFrame],
) -> Result<(), std::io::Error> {
    use std::io::Write;
    if let Some(parent) = path.parent() {
        if !parent.as_os_str().is_empty() {
            std::fs::create_dir_all(parent)?;
        }
    }
    let mut metadata = metadata;
    metadata.duration_ticks = frames
        .last()
        .zip(frames.first())
        .map(|(last, first)| last.tick.saturating_sub(first.tick))
        .unwrap_or(0);
    let header = ReplayHeader {
        version: REPLAY_FORMAT_VERSION,
        metadata,
        frame_count: frames.len() as u32,
    };
    let mut writer = std::io::BufWriter::new(std::fs::File::create(path)?);
    write_header(&mut writer, &header)?;
    let mut frames_out =
        flate2::write::ZlibEncoder::new(&mut writer, flate2::Compression::default());
    for frame in frames {
        write_frame(&mut frames_out, frame)?;
    }
    frames_out.finish()?;
    writer.flush()
}

/// Read just a replay file's header: its metadata and frame count, no frames.
pub fn read_replay_header(path: &std::path::Path) -> Result<ReplayHeader, std::io::Error> {
    use std::io::Read;
    let mut reader = std::io::BufReader::new(std::fs::File::open(path)?);
    let mut len = [0u8; 4];
    reader.read_exact(&mut len)?;
    let len = u32::from_le_bytes(len) as usize;
    // A header is a few kilobytes; refuse a length that says otherwise.
    if len > 16 * 1024 * 1024 {
        return Err(invalid_data(std::io::Error::other(
            "replay header too large",
        )));
    }
    let mut header_bytes = vec![0u8; len];
    reader.read_exact(&mut header_bytes)?;
    rmp_serde::from_slice(&header_bytes).map_err(invalid_data)
}

/// Read a whole replay file synchronously.
pub fn read_replay_file(
    path: &std::path::Path,
) -> Result<(ReplayMetadata, Vec<ReplayFrame>), std::io::Error> {
    use std::io::Read;
    let mut reader = std::io::BufReader::new(std::fs::File::open(path)?);
    let mut len = [0u8; 4];
    reader.read_exact(&mut len)?;
    let mut header_bytes = vec![0u8; u32::from_le_bytes(len) as usize];
    reader.read_exact(&mut header_bytes)?;
    let header: ReplayHeader = rmp_serde::from_slice(&header_bytes).map_err(invalid_data)?;
    let frames = if header.version >= ZLIB_FRAMES_VERSION {
        read_frames(
            &mut flate2::read::ZlibDecoder::new(reader),
            header.frame_count,
        )?
    } else {
        read_frames(&mut reader, header.frame_count)?
    };
    Ok((header.metadata, frames))
}

/// Manages replay recording and playback
pub struct ReplayManager {
    /// Directory where replays are stored
    replay_dir: PathBuf,

    /// Currently recording sessions (session_id -> ReplayRecorder)
    active_recordings: Arc<RwLock<HashMap<SessionId, ReplayRecorder>>>,
}

/// Records a single session's replay. The frames are not kept: each goes
/// over a bounded channel to a writer thread that compresses it into a
/// temporary file beside the replays, so a race of any length costs the
/// server the queue and nothing more. Stopping writes the header and copies
/// the compressed frames after it.
pub struct ReplayRecorder {
    session_id: SessionId,
    metadata: ReplayMetadata,
    sender: Option<std::sync::mpsc::SyncSender<ReplayFrame>>,
    writer: Option<std::thread::JoinHandle<std::io::Result<FrameSpan>>>,
    temp_path: PathBuf,
    /// Frames dropped because the writer had fallen a whole queue behind.
    dropped: u64,
}

/// What the writer thread wrote: how many frames, and the ticks they span.
#[derive(Debug, Default, Clone, Copy)]
struct FrameSpan {
    count: u32,
    first_tick: Option<u32>,
    last_tick: u32,
}

/// The writer thread: every frame the channel brings, compressed into the
/// file at `path`, until the recorder hangs up.
fn write_frames(
    path: &std::path::Path,
    frames: std::sync::mpsc::Receiver<ReplayFrame>,
) -> std::io::Result<FrameSpan> {
    let file = std::io::BufWriter::new(std::fs::File::create(path)?);
    let mut out = flate2::write::ZlibEncoder::new(file, flate2::Compression::fast());
    let mut span = FrameSpan::default();
    for frame in frames {
        write_frame(&mut out, &frame)?;
        span.count += 1;
        span.first_tick.get_or_insert(frame.tick);
        span.last_tick = frame.tick;
    }
    let mut file = out.finish()?;
    std::io::Write::flush(&mut file)?;
    Ok(span)
}

/// Stop a recording's writer and wait for it. `None` when it failed
/// (logged).
fn finish_writer(recorder: &mut ReplayRecorder) -> Option<FrameSpan> {
    recorder.sender = None;
    let result = recorder.writer.take()?.join();
    match result {
        Ok(Ok(span)) => Some(span),
        Ok(Err(e)) => {
            warn!(
                "Replay writer for session {} failed: {}",
                recorder.session_id, e
            );
            None
        }
        Err(_) => {
            warn!("Replay writer for session {} panicked", recorder.session_id);
            None
        }
    }
}

/// Throw a recording away: stop its writer and delete its frames.
fn discard(mut recorder: ReplayRecorder) {
    finish_writer(&mut recorder);
    let _ = std::fs::remove_file(&recorder.temp_path);
}

/// The replay file from a stopped recording: the header, then the
/// compressed frames the writer left in the temporary file.
fn assemble_replay(
    mut recorder: ReplayRecorder,
    replay_path: &std::path::Path,
) -> std::io::Result<()> {
    use std::io::Write;
    let span = finish_writer(&mut recorder);
    let result = (|| {
        let span = span.ok_or_else(|| std::io::Error::other("the replay writer failed"))?;
        let mut metadata = recorder.metadata.clone();
        metadata.duration_ticks = span
            .first_tick
            .map_or(0, |first| span.last_tick.saturating_sub(first));
        let header = ReplayHeader {
            version: REPLAY_FORMAT_VERSION,
            metadata,
            frame_count: span.count,
        };
        let mut out = std::io::BufWriter::new(std::fs::File::create(replay_path)?);
        write_header(&mut out, &header)?;
        std::io::copy(&mut std::fs::File::open(&recorder.temp_path)?, &mut out)?;
        out.flush()
    })();
    let _ = std::fs::remove_file(&recorder.temp_path);
    if recorder.dropped > 0 {
        warn!(
            "Replay for session {} is missing {} frame(s): the disk could not keep up",
            recorder.session_id, recorder.dropped
        );
    }
    result
}

impl ReplayManager {
    /// The folder the replays are written to.
    pub fn dir(&self) -> &std::path::Path {
        &self.replay_dir
    }

    pub fn new(replay_dir: PathBuf) -> Self {
        Self {
            replay_dir,
            active_recordings: Arc::new(RwLock::new(HashMap::new())),
        }
    }

    /// Start recording a session. A recording already running for it (a
    /// session that went back from racing without finishing) is dropped.
    pub async fn start_recording(&self, metadata: ReplayMetadata) {
        let session_id = metadata.session_id;
        let temp_path = self
            .replay_dir
            .join(format!(".recording_{}.frames", session_id));

        let (sender, receiver) = std::sync::mpsc::sync_channel(RECORDING_QUEUE_FRAMES);
        let writer = {
            let dir = self.replay_dir.clone();
            let path = temp_path.clone();
            std::thread::Builder::new()
                .name("replay-writer".into())
                .spawn(move || {
                    std::fs::create_dir_all(&dir)?;
                    write_frames(&path, receiver)
                })
        };
        let writer = match writer {
            Ok(handle) => handle,
            Err(e) => {
                warn!("Cannot record a replay for session {}: {}", session_id, e);
                return;
            }
        };
        let recorder = ReplayRecorder {
            session_id,
            metadata,
            sender: Some(sender),
            writer: Some(writer),
            temp_path,
            dropped: 0,
        };

        let replaced = self
            .active_recordings
            .write()
            .await
            .insert(session_id, recorder);
        if let Some(old) = replaced {
            tokio::task::spawn_blocking(move || discard(old));
        }
        debug!("Started recording replay for session {}", session_id);
    }

    /// Record a frame for a session. Never waits on the disk: a frame that
    /// finds the writer's queue full is dropped.
    pub async fn record_frame(&self, session_id: SessionId, tick: u32, telemetry: Telemetry) {
        if let Some(recorder) = self.active_recordings.write().await.get_mut(&session_id) {
            recorder.record_frame(tick, telemetry);
        }
    }

    /// Stop recording and save replay to disk. `race_start_tick` is the
    /// tick the lights went out, when the session raced.
    pub async fn stop_recording(
        &self,
        session_id: SessionId,
        race_start_tick: Option<u32>,
    ) -> Result<PathBuf, std::io::Error> {
        let recorder = self.active_recordings.write().await.remove(&session_id);

        let Some(mut recorder) = recorder else {
            warn!("No active recording for session {}", session_id);
            return Err(std::io::Error::new(
                std::io::ErrorKind::NotFound,
                "No active recording",
            ));
        };
        recorder.metadata.race_start_tick = race_start_tick;
        let filename = format!(
            "replay_{}_{}.bin",
            recorder.session_id,
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_secs()
        );
        let replay_path = self.replay_dir.join(filename);
        let path = replay_path.clone();
        tokio::task::spawn_blocking(move || assemble_replay(recorder, &path))
            .await
            .map_err(std::io::Error::other)??;
        debug!(
            "Saved replay for session {} to {:?}",
            session_id, replay_path
        );
        Ok(replay_path)
    }

    /// Drop the recordings of sessions that are gone without finishing
    /// (everyone left mid-race), deleting what they had written.
    pub async fn discard_recordings_except(&self, live: impl Fn(&SessionId) -> bool) {
        let mut recordings = self.active_recordings.write().await;
        let gone: Vec<SessionId> = recordings.keys().filter(|id| !live(id)).copied().collect();
        for session_id in gone {
            if let Some(recorder) = recordings.remove(&session_id) {
                debug!("Discarding the replay of ended session {}", session_id);
                tokio::task::spawn_blocking(move || discard(recorder));
            }
        }
    }

    /// Sessions being recorded.
    pub async fn recording_count(&self) -> usize {
        self.active_recordings.read().await.len()
    }

    /// Load a replay from disk
    pub async fn load_replay(&self, replay_path: PathBuf) -> Result<ReplayPlayer, std::io::Error> {
        let path = replay_path.clone();
        let (metadata, frames) = tokio::task::spawn_blocking(move || read_replay_file(&path))
            .await
            .map_err(std::io::Error::other)??;
        debug!(
            "Loaded replay from {:?} ({} frames)",
            replay_path,
            frames.len()
        );
        Ok(ReplayPlayer {
            metadata,
            frames,
            current_frame: 0,
        })
    }

    /// List all available replays
    pub async fn list_replays(&self) -> Result<Vec<ReplayMetadata>, std::io::Error> {
        let mut replays = Vec::new();

        if !self.replay_dir.exists() {
            return Ok(replays);
        }

        let mut entries = fs::read_dir(&self.replay_dir).await?;

        while let Some(entry) = entries.next_entry().await? {
            let path = entry.path();

            if path.extension().and_then(|s| s.to_str()) == Some("bin") {
                // Read just the header to get metadata
                match self.read_replay_metadata(&path).await {
                    Ok(metadata) => replays.push(metadata),
                    Err(e) => {
                        warn!("Failed to read replay metadata from {:?}: {}", path, e);
                    }
                }
            }
        }

        Ok(replays)
    }

    /// Read just the metadata from a replay file
    async fn read_replay_metadata(&self, path: &PathBuf) -> Result<ReplayMetadata, std::io::Error> {
        use tokio::io::{AsyncReadExt, BufReader};

        let file = File::open(path).await?;
        let mut reader = BufReader::new(file);

        // Read header
        let mut header_len_bytes = [0u8; 4];
        reader.read_exact(&mut header_len_bytes).await?;
        let header_len = u32::from_le_bytes(header_len_bytes) as usize;

        let mut header_bytes = vec![0u8; header_len];
        reader.read_exact(&mut header_bytes).await?;

        let header: ReplayHeader = rmp_serde::from_slice(&header_bytes)
            .map_err(|e| std::io::Error::new(std::io::ErrorKind::InvalidData, e))?;

        Ok(header.metadata)
    }
}

impl ReplayRecorder {
    pub fn record_frame(&mut self, tick: u32, telemetry: Telemetry) {
        let Some(sender) = &self.sender else {
            return;
        };
        match sender.try_send(ReplayFrame { tick, telemetry }) {
            Ok(()) => {}
            Err(std::sync::mpsc::TrySendError::Full(_)) => {
                if self.dropped == 0 {
                    warn!(
                        "Replay writer for session {} is behind; dropping frames",
                        self.session_id
                    );
                }
                self.dropped += 1;
            }
            // The writer failed; stopping reports it.
            Err(std::sync::mpsc::TrySendError::Disconnected(_)) => self.sender = None,
        }
    }
}

/// Plays back a recorded replay
pub struct ReplayPlayer {
    metadata: ReplayMetadata,
    frames: Vec<ReplayFrame>,
    current_frame: usize,
}

impl ReplayPlayer {
    /// Get replay metadata
    pub fn metadata(&self) -> &ReplayMetadata {
        &self.metadata
    }

    /// Get current frame
    pub fn current_frame(&self) -> usize {
        self.current_frame
    }

    /// Get total frame count
    pub fn frame_count(&self) -> usize {
        self.frames.len()
    }

    /// Get next frame
    pub fn next_frame(&mut self) -> Option<&ReplayFrame> {
        if self.current_frame < self.frames.len() {
            let frame = &self.frames[self.current_frame];
            self.current_frame += 1;
            Some(frame)
        } else {
            None
        }
    }

    /// Seek to a specific frame
    pub fn seek(&mut self, frame: usize) {
        self.current_frame = frame.min(self.frames.len());
    }

    /// Reset to beginning
    pub fn reset(&mut self) {
        self.current_frame = 0;
    }

    /// Get frame at specific index
    pub fn get_frame(&self, index: usize) -> Option<&ReplayFrame> {
        self.frames.get(index)
    }

    /// Every frame, in tick order.
    pub fn frames(&self) -> &[ReplayFrame] {
        &self.frames
    }

    /// Check if replay has ended
    pub fn is_finished(&self) -> bool {
        self.current_frame >= self.frames.len()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::data::SessionState;
    use tempfile::TempDir;
    use uuid::Uuid;

    #[tokio::test]
    async fn test_replay_recording() {
        let temp_dir = TempDir::new().unwrap();
        let manager = ReplayManager::new(temp_dir.path().to_path_buf());

        let session_id = Uuid::new_v4();
        let metadata = ReplayMetadata {
            session_id,
            track_config_id: Uuid::new_v4(),
            track_name: "Test Track".to_string(),
            recorded_at: 123456789,
            duration_ticks: 0,
            tick_rate: 240,
            participants: vec![],
            conditions: SessionConditions::DEFAULT,
            race_start_tick: None,
            track_stem: None,
            track_length_m: 0.0,
        };

        manager.start_recording(metadata).await;

        // Record some frames
        for tick in 0..10 {
            let telemetry = Telemetry {
                server_tick: tick,
                session_state: SessionState::Racing,
                game_mode: GameMode::FreePractice,
                countdown_ms: None,
                car_states: vec![],
            };

            manager.record_frame(session_id, tick, telemetry).await;
        }

        // Stop and save
        let replay_path = manager.stop_recording(session_id, None).await.unwrap();
        assert!(replay_path.exists());

        // Load and verify
        let player = manager.load_replay(replay_path).await.unwrap();
        assert_eq!(player.frame_count(), 10);
    }

    #[tokio::test]
    async fn test_replay_playback() {
        let temp_dir = TempDir::new().unwrap();
        let manager = ReplayManager::new(temp_dir.path().to_path_buf());

        let session_id = Uuid::new_v4();
        let metadata = ReplayMetadata {
            session_id,
            track_config_id: Uuid::new_v4(),
            track_name: "Test Track".to_string(),
            recorded_at: 123456789,
            duration_ticks: 0,
            tick_rate: 240,
            participants: vec![],
            conditions: SessionConditions::DEFAULT,
            race_start_tick: None,
            track_stem: None,
            track_length_m: 0.0,
        };

        manager.start_recording(metadata).await;

        // Record frames
        for tick in 0..5 {
            let telemetry = Telemetry {
                server_tick: tick,
                session_state: SessionState::Racing,
                game_mode: GameMode::FreePractice,
                countdown_ms: None,
                car_states: vec![],
            };
            manager.record_frame(session_id, tick, telemetry).await;
        }

        let replay_path = manager.stop_recording(session_id, None).await.unwrap();
        let mut player = manager.load_replay(replay_path).await.unwrap();

        // Playback
        let mut frame_count = 0;
        while let Some(_frame) = player.next_frame() {
            frame_count += 1;
        }

        assert_eq!(frame_count, 5);
        assert!(player.is_finished());

        // Reset and replay
        player.reset();
        assert_eq!(player.current_frame(), 0);
    }

    fn test_metadata(session_id: SessionId) -> ReplayMetadata {
        ReplayMetadata {
            session_id,
            track_config_id: Uuid::nil(),
            track_name: "Test Track".to_string(),
            recorded_at: 0,
            duration_ticks: 0,
            tick_rate: 240,
            participants: vec![],
            conditions: SessionConditions::DEFAULT,
            race_start_tick: None,
            track_stem: None,
            track_length_m: 0.0,
        }
    }

    fn test_telemetry(tick: u32) -> Telemetry {
        Telemetry {
            server_tick: tick,
            session_state: SessionState::Racing,
            game_mode: GameMode::Race,
            countdown_ms: None,
            car_states: vec![],
        }
    }

    /// A recording goes to disk as it is made: the frames are in a
    /// temporary file while it runs, and only the finished replay is left
    /// once it stops, compressed, every frame in order with its span.
    #[tokio::test]
    async fn a_recording_streams_to_disk_and_leaves_only_the_replay() {
        let temp_dir = TempDir::new().unwrap();
        let manager = ReplayManager::new(temp_dir.path().to_path_buf());
        let session_id = Uuid::from_u128(7);
        manager.start_recording(test_metadata(session_id)).await;
        for tick in (100..100 + 4 * 1000).step_by(4) {
            manager
                .record_frame(session_id, tick, test_telemetry(tick))
                .await;
        }
        let path = manager.stop_recording(session_id, Some(100)).await.unwrap();

        let names: Vec<String> = std::fs::read_dir(temp_dir.path())
            .unwrap()
            .map(|e| e.unwrap().file_name().to_string_lossy().into_owned())
            .collect();
        assert_eq!(names.len(), 1, "only the replay is left: {names:?}");

        let (meta, frames) = read_replay_file(&path).unwrap();
        assert_eq!(frames.len(), 1000);
        assert!(frames.windows(2).all(|w| w[1].tick == w[0].tick + 4));
        assert_eq!(meta.duration_ticks, 3996);
        assert_eq!(meta.race_start_tick, Some(100));
        assert_eq!(manager.recording_count().await, 0);
    }

    /// A version 2 file, frames uncompressed, still reads.
    #[test]
    fn a_version_2_replay_still_reads() {
        use std::io::Write;
        let temp_dir = TempDir::new().unwrap();
        let path = temp_dir.path().join("old.bin");
        let frames: Vec<ReplayFrame> = (0..3)
            .map(|tick| ReplayFrame {
                tick,
                telemetry: test_telemetry(tick),
            })
            .collect();
        let mut file = std::fs::File::create(&path).unwrap();
        let header = ReplayHeader {
            version: 2,
            metadata: test_metadata(Uuid::nil()),
            frame_count: 3,
        };
        write_header(&mut file, &header).unwrap();
        for frame in &frames {
            write_frame(&mut file, frame).unwrap();
        }
        file.flush().unwrap();
        drop(file);

        let (_, back) = read_replay_file(&path).unwrap();
        assert_eq!(back.iter().map(|f| f.tick).collect::<Vec<_>>(), [0, 1, 2]);
    }

    /// A session that ends without finishing takes its recording with it.
    #[tokio::test]
    async fn an_abandoned_recording_is_discarded() {
        let temp_dir = TempDir::new().unwrap();
        let manager = ReplayManager::new(temp_dir.path().to_path_buf());
        let session_id = Uuid::from_u128(8);
        manager.start_recording(test_metadata(session_id)).await;
        manager.record_frame(session_id, 1, test_telemetry(1)).await;
        manager.discard_recordings_except(|_| false).await;
        assert_eq!(manager.recording_count().await, 0);
        // The writer is stopped and its file deleted off the loop.
        for _ in 0..200 {
            if std::fs::read_dir(temp_dir.path()).unwrap().next().is_none() {
                return;
            }
            tokio::time::sleep(std::time::Duration::from_millis(10)).await;
        }
        panic!("the recording's frames were not deleted");
    }
}
