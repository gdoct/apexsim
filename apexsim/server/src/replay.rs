//! The replay format of the offline tools (`apexsim-replay simulate`, `find`,
//! `cut`, `convert`): every frame of full telemetry, MessagePack with named
//! fields, compressed. A server's own recordings of live sessions are
//! spectator streams instead (`crate::stream_recorder`, `.apxs`).

use crate::data::*;
use crate::network::Telemetry;
use serde::{Deserialize, Serialize};

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
    /// The car's fastest legal lap, ms, as the session ended.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub best_lap_ms: Option<u32>,
    /// Laps the car completed.
    #[serde(default)]
    pub laps: u16,
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

#[cfg(test)]
mod tests {
    use super::*;
    use crate::data::SessionState;
    use tempfile::TempDir;
    use uuid::Uuid;

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

    /// A file the offline tools write reads back, frames in order, with its
    /// span in the header.
    #[test]
    fn a_replay_file_round_trips() {
        let temp_dir = TempDir::new().unwrap();
        let path = temp_dir.path().join("out").join("race.bin");
        let frames: Vec<ReplayFrame> = (100..100 + 4 * 1000)
            .step_by(4)
            .map(|tick| ReplayFrame {
                tick,
                telemetry: test_telemetry(tick),
            })
            .collect();
        write_replay_file(&path, test_metadata(Uuid::from_u128(7)), &frames).unwrap();
        let (meta, back) = read_replay_file(&path).unwrap();
        assert_eq!(back.len(), 1000);
        assert!(back.windows(2).all(|w| w[1].tick == w[0].tick + 4));
        assert_eq!(meta.duration_ticks, 3996);
        assert_eq!(read_replay_header(&path).unwrap().frame_count, 1000);
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
}
