//! A track's physics files as one bundle, for a predicting client
//! (docs/proposals/client-prediction.md, phase 4).
//!
//! The client steps its own car with the server's car step (`server/ffi`),
//! which reads the track YAML and the baked sidecars beside it: the ground,
//! curbs, walls, pit lane and, when the session drives on it, the road mesh.
//! A packaged client ships none of them, and a server's custom track is on
//! no client at all, so the server hands its own copy out: the files it
//! loaded the track from, read once (`TrackFiles::read`), deflated into a
//! bundle kept beside the loaded track (`crate::track_content`) and sent in
//! chunks on request (`ClientMessage::RequestTrackBundle`).
//!
//! **The checksum** ([`TrackFiles::checksum`]) covers the YAML and every
//! sidecar: CRC-32 over the bundle's magic and version, and for each file in
//! order a presence byte and its own CRC (the YAML's content checksum,
//! carriage returns dropped as `ContentCrc`'s are; a sidecar's over every
//! byte). A client compares it against its own copy's (the library's
//! `apx_track_files_crc`, this function) and the bundle's, so a sidecar
//! baked another day turns prediction off as a changed YAML does.
//!
//! **The bytes**: `APXT`, the version (u8), the raw length (u32 LE), then
//! the six files deflated (zlib) as `[present u8][len u32 LE][bytes]` each.

use std::io::{Read, Write};
use std::path::Path;

use flate2::read::ZlibDecoder;
use flate2::write::ZlibEncoder;
use flate2::Compression;

use crate::config::RoadContactMode;
use crate::content_crc::{content_crc, crc32};

/// The bundle format's version.
pub const BUNDLE_VERSION: u8 = 1;

const MAGIC: [u8; 4] = *b"APXT";

/// The bytes of one `TrackBundleChunk`: small enough that a chunk queued on
/// the TCP connection holds other messages up only briefly (0.2 s at
/// 10 Mbit/s), large enough that the Nordschleife is under 40 of them.
pub const CHUNK_BYTES: usize = 256 * 1024;

/// The largest bundle a reader inflates: well over the Nordschleife's 20 MB,
/// short of what a hostile bundle could ask a client to allocate.
pub const MAX_RAW_BYTES: usize = 256 * 1024 * 1024;

/// A track's physics files, as the car step reads them.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct TrackFiles {
    pub yaml: Vec<u8>,
    pub ground: Option<Vec<u8>>,
    pub curbs: Option<Vec<u8>>,
    pub walls: Option<Vec<u8>>,
    pub pit: Option<Vec<u8>>,
    /// Only when the session drives on the road mesh.
    pub road: Option<Vec<u8>>,
}

/// Why a bundle was refused.
#[derive(Debug, thiserror::Error, PartialEq, Eq)]
pub enum BundleError {
    #[error("not a track bundle")]
    NotABundle,
    #[error("track bundle version {0}, this build reads {BUNDLE_VERSION}")]
    Version(u8),
    #[error("the track bundle is damaged: {0}")]
    Damaged(String),
    #[error("the track bundle's checksum is {found:08X}, not the {expected:08X} asked for")]
    Checksum { expected: u32, found: u32 },
}

/// The sidecar beside `track_path` with this suffix (`<Stem>.<suffix>`).
fn sidecar(track_path: &Path, suffix: &str) -> std::path::PathBuf {
    let stem = track_path
        .file_stem()
        .and_then(|s| s.to_str())
        .unwrap_or("track");
    track_path.with_file_name(format!("{stem}.{suffix}"))
}

fn read_optional(path: &Path) -> Option<Vec<u8>> {
    std::fs::read(path).ok()
}

impl TrackFiles {
    /// The YAML at `track_path` and the sidecars beside it; the road mesh
    /// only for [`RoadContactMode::Mesh`]. A sidecar that is not there (or
    /// cannot be read) is absent, as the track loader treats it.
    pub fn read(track_path: &Path, road_contact: RoadContactMode) -> std::io::Result<Self> {
        let yaml = std::fs::read(track_path)?;
        Ok(Self {
            yaml,
            ..Self::read_sidecars(track_path, road_contact)
        })
    }

    /// The sidecars beside `track_path`, without the YAML.
    pub fn read_sidecars(track_path: &Path, road_contact: RoadContactMode) -> Self {
        Self {
            yaml: Vec::new(),
            ground: read_optional(&sidecar(track_path, "ground.msgpack")),
            curbs: read_optional(&sidecar(track_path, "curbs.msgpack")),
            walls: read_optional(&sidecar(track_path, "walls.msgpack")),
            pit: read_optional(&sidecar(track_path, "pit.msgpack")),
            road: (road_contact == RoadContactMode::Mesh)
                .then(|| read_optional(&sidecar(track_path, "road.msgpack")))
                .flatten(),
        }
    }

    fn sidecars(&self) -> [&Option<Vec<u8>>; 5] {
        [
            &self.ground,
            &self.curbs,
            &self.walls,
            &self.pit,
            &self.road,
        ]
    }

    /// The checksum over the YAML and every sidecar (see the module docs).
    pub fn checksum(&self) -> u32 {
        let mut digest = Vec::with_capacity(4 + 1 + 6 * 5);
        digest.extend_from_slice(&MAGIC);
        digest.push(BUNDLE_VERSION);
        digest.push(1);
        digest.extend_from_slice(&content_crc(&self.yaml).to_le_bytes());
        for file in self.sidecars() {
            match file {
                Some(bytes) => {
                    digest.push(1);
                    digest.extend_from_slice(&crc32(bytes).to_le_bytes());
                }
                None => digest.push(0),
            }
        }
        crc32(&digest)
    }

    /// Every file's bytes together, uncompressed.
    pub fn raw_len(&self) -> usize {
        self.yaml.len()
            + self
                .sidecars()
                .iter()
                .map(|f| f.as_ref().map_or(0, Vec::len))
                .sum::<usize>()
    }

    /// The bundle's bytes: the header, then the files deflated.
    pub fn encode(&self) -> Vec<u8> {
        let raw = 6 * 5 + self.raw_len();
        let mut out = Vec::with_capacity(raw / 2 + 16);
        out.extend_from_slice(&MAGIC);
        out.push(BUNDLE_VERSION);
        out.extend_from_slice(&(raw as u32).to_le_bytes());
        // Fast: a server deflates a bundle each time it loads a track, the
        // Nordschleife's 20 MB among them; the best level saves a few
        // percent more.
        let mut z = ZlibEncoder::new(out, Compression::fast());
        let mut put = |file: Option<&Vec<u8>>| {
            let bytes = file.map_or(&[][..], Vec::as_slice);
            // Writing into a Vec does not fail.
            z.write_all(&[file.is_some() as u8]).expect("deflate");
            z.write_all(&(bytes.len() as u32).to_le_bytes())
                .expect("deflate");
            z.write_all(bytes).expect("deflate");
        };
        put(Some(&self.yaml));
        for file in self.sidecars() {
            put(file.as_ref());
        }
        z.finish().expect("deflate")
    }

    /// The files a bundle holds, refused unless its checksum is `expected`
    /// (none checks nothing).
    pub fn decode(bytes: &[u8], expected: Option<u32>) -> Result<Self, BundleError> {
        if bytes.len() < 9 || bytes[..4] != MAGIC {
            return Err(BundleError::NotABundle);
        }
        if bytes[4] != BUNDLE_VERSION {
            return Err(BundleError::Version(bytes[4]));
        }
        let raw_len = u32::from_le_bytes([bytes[5], bytes[6], bytes[7], bytes[8]]) as usize;
        if raw_len > MAX_RAW_BYTES {
            return Err(BundleError::Damaged(format!("{raw_len} bytes inflated")));
        }
        let mut raw = Vec::with_capacity(raw_len);
        ZlibDecoder::new(&bytes[9..])
            .take(raw_len as u64 + 1)
            .read_to_end(&mut raw)
            .map_err(|e| BundleError::Damaged(e.to_string()))?;
        if raw.len() != raw_len {
            return Err(BundleError::Damaged(format!(
                "{} bytes inflated, the header says {raw_len}",
                raw.len()
            )));
        }
        let mut at = 0usize;
        let mut next = || -> Result<Option<Vec<u8>>, BundleError> {
            let short = || BundleError::Damaged("a file runs past the end".into());
            let head = raw.get(at..at + 5).ok_or_else(short)?;
            let present = head[0] != 0;
            let len = u32::from_le_bytes([head[1], head[2], head[3], head[4]]) as usize;
            let body = raw.get(at + 5..at + 5 + len).ok_or_else(short)?;
            at += 5 + len;
            Ok(present.then(|| body.to_vec()))
        };
        let yaml = next()?.ok_or_else(|| BundleError::Damaged("no YAML".into()))?;
        let files = Self {
            yaml,
            ground: next()?,
            curbs: next()?,
            walls: next()?,
            pit: next()?,
            road: next()?,
        };
        if at != raw.len() {
            return Err(BundleError::Damaged("bytes after the last file".into()));
        }
        if let Some(expected) = expected {
            let found = files.checksum();
            if found != expected {
                return Err(BundleError::Checksum { expected, found });
            }
        }
        Ok(files)
    }
}

/// A loaded track's bundle, as the server hands it out.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TrackBundle {
    /// [`TrackFiles::checksum`] of the files in it.
    pub crc: u32,
    /// [`TrackFiles::encode`]'s bytes.
    pub bytes: Vec<u8>,
    /// The files' bytes together, uncompressed (for the log).
    pub raw_len: usize,
}

impl TrackBundle {
    pub fn of(files: &TrackFiles) -> Self {
        Self {
            crc: files.checksum(),
            bytes: files.encode(),
            raw_len: files.raw_len(),
        }
    }

    /// How many [`CHUNK_BYTES`] chunks it goes in.
    pub fn chunk_count(&self) -> u32 {
        self.bytes.len().div_ceil(CHUNK_BYTES) as u32
    }

    /// Chunk `index`, or none past the last.
    pub fn chunk(&self, index: u32) -> Option<&[u8]> {
        let start = (index as usize).checked_mul(CHUNK_BYTES)?;
        if start >= self.bytes.len() {
            return None;
        }
        Some(&self.bytes[start..(start + CHUNK_BYTES).min(self.bytes.len())])
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn files() -> TrackFiles {
        TrackFiles {
            yaml: b"name: Square\r\nnodes: []\r\n".to_vec(),
            ground: Some(vec![1, 2, 3, 0x0D, 4]),
            curbs: None,
            walls: Some((0..300_000u32).map(|i| (i % 251) as u8).collect()),
            pit: Some(Vec::new()),
            road: None,
        }
    }

    #[test]
    fn a_bundle_round_trips() {
        let files = files();
        let bundle = TrackBundle::of(&files);
        assert!(bundle.bytes.len() < files.raw_len(), "deflated");
        assert_eq!(
            TrackFiles::decode(&bundle.bytes, Some(bundle.crc)).unwrap(),
            files
        );
        // In chunks, as it goes over the wire.
        let mut joined = Vec::new();
        for i in 0..bundle.chunk_count() {
            joined.extend_from_slice(bundle.chunk(i).unwrap());
        }
        assert!(bundle.chunk(bundle.chunk_count()).is_none());
        assert_eq!(joined, bundle.bytes);
    }

    #[test]
    fn the_checksum_covers_every_file() {
        let base = files().checksum();
        let mut yaml = files();
        yaml.yaml.push(b'x');
        let mut crlf = files();
        crlf.yaml = b"name: Square\nnodes: []\n".to_vec();
        let mut sidecar = files();
        sidecar.ground.as_mut().unwrap()[3] = 0x0A;
        let mut gone = files();
        gone.pit = None;
        let mut road = files();
        road.road = Some(vec![9]);
        assert_ne!(yaml.checksum(), base, "the YAML");
        assert_eq!(
            crlf.checksum(),
            base,
            "line endings in the YAML do not count"
        );
        assert_ne!(
            sidecar.checksum(),
            base,
            "a sidecar's carriage return is data"
        );
        assert_ne!(
            gone.checksum(),
            base,
            "an empty sidecar is not a missing one"
        );
        assert_ne!(road.checksum(), base, "the road mesh");
    }

    #[test]
    fn a_wrong_or_damaged_bundle_is_refused() {
        let bundle = TrackBundle::of(&files());
        assert!(matches!(
            TrackFiles::decode(&bundle.bytes, Some(bundle.crc ^ 1)),
            Err(BundleError::Checksum { .. })
        ));
        let mut cut = bundle.bytes.clone();
        cut.truncate(cut.len() - 10);
        assert!(matches!(
            TrackFiles::decode(&cut, None),
            Err(BundleError::Damaged(_))
        ));
        assert_eq!(
            TrackFiles::decode(b"APXC\x01\0\0\0\0", None),
            Err(BundleError::NotABundle)
        );
        let mut later = bundle.bytes.clone();
        later[4] = 9;
        assert_eq!(
            TrackFiles::decode(&later, None),
            Err(BundleError::Version(9))
        );
        let mut bomb = bundle.bytes.clone();
        bomb[5..9].copy_from_slice(&u32::MAX.to_le_bytes());
        assert!(matches!(
            TrackFiles::decode(&bomb, None),
            Err(BundleError::Damaged(_))
        ));
    }

    #[test]
    fn reading_skips_the_road_mesh_off_the_mesh() {
        let dir = tempfile::tempdir().unwrap();
        let yaml = dir.path().join("Square.yaml");
        std::fs::write(&yaml, b"name: Square\n").unwrap();
        std::fs::write(dir.path().join("Square.road.msgpack"), [7u8; 3]).unwrap();
        std::fs::write(dir.path().join("Square.walls.msgpack"), [8u8; 2]).unwrap();
        let mesh = TrackFiles::read(&yaml, RoadContactMode::Mesh).unwrap();
        let line = TrackFiles::read(&yaml, RoadContactMode::Centerline).unwrap();
        assert_eq!(mesh.road, Some(vec![7; 3]));
        assert_eq!(line.road, None);
        assert_eq!(line.walls, Some(vec![8; 2]));
        assert_eq!(line.ground, None);
        assert_ne!(mesh.checksum(), line.checksum());
    }
}
