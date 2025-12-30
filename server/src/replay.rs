use crate::data::*;
use crate::network::Telemetry;
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::Arc;
use tokio::fs::{self, File};
use tokio::io::{AsyncWriteExt, BufWriter};
use tokio::sync::RwLock;
use tracing::{info, warn};

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
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ReplayParticipant {
    pub player_id: PlayerId,
    pub player_name: String,
    pub car_config_id: CarConfigId,
    pub finish_position: Option<u8>,
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

/// Manages replay recording and playback
pub struct ReplayManager {
    /// Directory where replays are stored
    replay_dir: PathBuf,

    /// Currently recording sessions (session_id -> ReplayRecorder)
    active_recordings: Arc<RwLock<HashMap<SessionId, ReplayRecorder>>>,
}

/// Records a single session's replay
pub struct ReplayRecorder {
    session_id: SessionId,
    metadata: ReplayMetadata,
    frames: Vec<ReplayFrame>,
    tick_rate: u16,
}

impl ReplayManager {
    pub fn new(replay_dir: PathBuf) -> Self {
        Self {
            replay_dir,
            active_recordings: Arc::new(RwLock::new(HashMap::new())),
        }
    }

    /// Start recording a session
    pub async fn start_recording(&self, metadata: ReplayMetadata) {
        let session_id = metadata.session_id;
        let tick_rate = metadata.tick_rate;

        let recorder = ReplayRecorder {
            session_id,
            metadata,
            frames: Vec::new(),
            tick_rate,
        };

        self.active_recordings.write().await.insert(session_id, recorder);
        info!("Started recording replay for session {}", session_id);
    }

    /// Record a frame for a session
    pub async fn record_frame(&self, session_id: SessionId, tick: u32, telemetry: Telemetry) {
        if let Some(recorder) = self.active_recordings.write().await.get_mut(&session_id) {
            recorder.record_frame(tick, telemetry);
        }
    }

    /// Stop recording and save replay to disk
    pub async fn stop_recording(&self, session_id: SessionId) -> Result<PathBuf, std::io::Error> {
        let recorder = self.active_recordings.write().await.remove(&session_id);

        if let Some(recorder) = recorder {
            let replay_path = self.save_replay(recorder).await?;
            info!("Saved replay for session {} to {:?}", session_id, replay_path);
            Ok(replay_path)
        } else {
            warn!("No active recording for session {}", session_id);
            Err(std::io::Error::new(
                std::io::ErrorKind::NotFound,
                "No active recording",
            ))
        }
    }

    /// Save replay to disk
    async fn save_replay(&self, recorder: ReplayRecorder) -> Result<PathBuf, std::io::Error> {
        // Create replay directory if it doesn't exist
        fs::create_dir_all(&self.replay_dir).await?;

        // Generate filename
        let filename = format!(
            "replay_{}_{}.bin",
            recorder.session_id,
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_secs()
        );

        let replay_path = self.replay_dir.join(filename);

        // Create replay header
        let mut metadata = recorder.metadata;
        metadata.duration_ticks = recorder.frames.len() as u32;

        let header = ReplayHeader {
            version: 1,
            metadata,
            frame_count: recorder.frames.len() as u32,
        };

        // Write to file
        let file = File::create(&replay_path).await?;
        let mut writer = BufWriter::new(file);

        // Write header
        let header_bytes = bincode::serialize(&header)
            .map_err(|e| std::io::Error::new(std::io::ErrorKind::InvalidData, e))?;
        let header_len = header_bytes.len() as u32;
        writer.write_all(&header_len.to_le_bytes()).await?;
        writer.write_all(&header_bytes).await?;

        // Write frames
        for frame in &recorder.frames {
            let frame_bytes = bincode::serialize(frame)
                .map_err(|e| std::io::Error::new(std::io::ErrorKind::InvalidData, e))?;
            let frame_len = frame_bytes.len() as u32;
            writer.write_all(&frame_len.to_le_bytes()).await?;
            writer.write_all(&frame_bytes).await?;
        }

        writer.flush().await?;

        Ok(replay_path)
    }

    /// Load a replay from disk
    pub async fn load_replay(&self, replay_path: PathBuf) -> Result<ReplayPlayer, std::io::Error> {
        use tokio::io::{AsyncReadExt, BufReader};

        let file = File::open(&replay_path).await?;
        let mut reader = BufReader::new(file);

        // Read header
        let mut header_len_bytes = [0u8; 4];
        reader.read_exact(&mut header_len_bytes).await?;
        let header_len = u32::from_le_bytes(header_len_bytes) as usize;

        let mut header_bytes = vec![0u8; header_len];
        reader.read_exact(&mut header_bytes).await?;

        let header: ReplayHeader = bincode::deserialize(&header_bytes)
            .map_err(|e| std::io::Error::new(std::io::ErrorKind::InvalidData, e))?;

        // Read frames
        let mut frames = Vec::with_capacity(header.frame_count as usize);
        for _ in 0..header.frame_count {
            let mut frame_len_bytes = [0u8; 4];
            reader.read_exact(&mut frame_len_bytes).await?;
            let frame_len = u32::from_le_bytes(frame_len_bytes) as usize;

            let mut frame_bytes = vec![0u8; frame_len];
            reader.read_exact(&mut frame_bytes).await?;

            let frame: ReplayFrame = bincode::deserialize(&frame_bytes)
                .map_err(|e| std::io::Error::new(std::io::ErrorKind::InvalidData, e))?;

            frames.push(frame);
        }

        info!("Loaded replay from {:?} ({} frames)", replay_path, frames.len());

        Ok(ReplayPlayer {
            metadata: header.metadata,
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

        let header: ReplayHeader = bincode::deserialize(&header_bytes)
            .map_err(|e| std::io::Error::new(std::io::ErrorKind::InvalidData, e))?;

        Ok(header.metadata)
    }
}

impl ReplayRecorder {
    pub fn record_frame(&mut self, tick: u32, telemetry: Telemetry) {
        self.frames.push(ReplayFrame { tick, telemetry });
    }

    pub fn get_frame_count(&self) -> usize {
        self.frames.len()
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
        };

        manager.start_recording(metadata).await;

        // Record some frames
        for tick in 0..10 {
            let telemetry = Telemetry {
                server_tick: tick,
                session_state: SessionState::Racing,
                countdown_ms: None,
                car_states: vec![],
            };

            manager.record_frame(session_id, tick, telemetry).await;
        }

        // Stop and save
        let replay_path = manager.stop_recording(session_id).await.unwrap();
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
        };

        manager.start_recording(metadata).await;

        // Record frames
        for tick in 0..5 {
            let telemetry = Telemetry {
                server_tick: tick,
                session_state: SessionState::Racing,
                countdown_ms: None,
                car_states: vec![],
            };
            manager.record_frame(session_id, tick, telemetry).await;
        }

        let replay_path = manager.stop_recording(session_id).await.unwrap();
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
}
