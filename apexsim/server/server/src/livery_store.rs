//! Players' own liveries as the server keeps them (N11, `crate::livery`):
//! uploads assembled per connection (`UploadLivery`, chunks in order), then
//! checked and kept by the SHA-256 of their bytes for any client to fetch
//! (`RequestLivery`). The store holds `MAX_STORE_BYTES` at most; past that
//! the oldest kept go first, and a client asking for one gone is told so
//! (`LiveryChunk.Chunks` 0) and paints the car's works livery.

use crate::data::ConnectionId;
use crate::livery::{
    LiveryError, LiveryPackage, LIVERY_CHUNK_BYTES, MAX_LIVERY_BYTES, MAX_LIVERY_CHUNKS,
};
use std::collections::{HashMap, VecDeque};
use std::sync::Arc;

/// The liveries kept, bytes at most.
pub const MAX_STORE_BYTES: usize = 64 << 20;

/// The SHA-256 of `bytes`, lowercase hex: a livery's name.
pub fn hash_of(bytes: &[u8]) -> String {
    ring::digest::digest(&ring::digest::SHA256, bytes)
        .as_ref()
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect()
}

/// A livery being uploaded on one connection.
struct Upload {
    hash: String,
    chunks: u32,
    next: u32,
    bytes: Vec<u8>,
}

/// Why an upload is refused, as the `Error` code and message the client gets.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Refused {
    pub code: u16,
    pub message: String,
}

impl Refused {
    fn bad(message: impl Into<String>) -> Self {
        Self {
            code: 400,
            message: message.into(),
        }
    }
}

impl From<LiveryError> for Refused {
    fn from(e: LiveryError) -> Self {
        Self {
            code: if matches!(e, LiveryError::TooLarge(_)) {
                413
            } else {
                400
            },
            message: e.to_string(),
        }
    }
}

/// What a chunk of an upload came to.
#[derive(Debug, PartialEq)]
pub enum Received {
    /// More chunks to come.
    Partial,
    /// The last chunk: the whole livery, checked and kept under its hash.
    Done {
        hash: String,
        package: LiveryPackage,
    },
}

#[derive(Default)]
pub struct LiveryStore {
    kept: HashMap<String, Arc<Vec<u8>>>,
    /// The hashes kept, oldest first.
    order: VecDeque<String>,
    bytes: usize,
    uploads: HashMap<ConnectionId, Upload>,
}

impl LiveryStore {
    /// One chunk of an upload on `connection`. Chunk 0 starts a new one
    /// (dropping one under way); any other must be the next. On the last the
    /// bytes must hash to `hash` and read as a package within the caps.
    pub fn receive(
        &mut self,
        connection: ConnectionId,
        hash: &str,
        chunk: u32,
        chunks: u32,
        bytes: &[u8],
    ) -> Result<Received, Refused> {
        if chunks == 0 || chunks > MAX_LIVERY_CHUNKS || chunk >= chunks {
            self.uploads.remove(&connection);
            return Err(Refused {
                code: 413,
                message: format!("A livery comes in 1 to {MAX_LIVERY_CHUNKS} chunks"),
            });
        }
        if bytes.len() > LIVERY_CHUNK_BYTES {
            self.uploads.remove(&connection);
            return Err(Refused {
                code: 413,
                message: format!("A livery's chunk is {LIVERY_CHUNK_BYTES} bytes at most"),
            });
        }
        if chunk == 0 {
            self.uploads.insert(
                connection,
                Upload {
                    hash: hash.to_string(),
                    chunks,
                    next: 0,
                    bytes: Vec::new(),
                },
            );
        }
        let Some(upload) = self.uploads.get_mut(&connection) else {
            return Err(Refused::bad("A livery's chunks come in order from 0"));
        };
        if upload.next != chunk || upload.chunks != chunks || upload.hash != hash {
            self.uploads.remove(&connection);
            return Err(Refused::bad("A livery's chunks come in order from 0"));
        }
        if upload.bytes.len() + bytes.len() > MAX_LIVERY_BYTES {
            let total = upload.bytes.len() + bytes.len();
            self.uploads.remove(&connection);
            return Err(LiveryError::TooLarge(total).into());
        }
        upload.bytes.extend_from_slice(bytes);
        upload.next += 1;
        if upload.next < upload.chunks {
            return Ok(Received::Partial);
        }
        let upload = self
            .uploads
            .remove(&connection)
            .expect("the upload is there");
        if hash_of(&upload.bytes) != upload.hash {
            return Err(Refused::bad("The livery does not match its hash"));
        }
        let package = LiveryPackage::check(&upload.bytes)?;
        self.keep(upload.hash.clone(), upload.bytes);
        Ok(Received::Done {
            hash: upload.hash,
            package,
        })
    }

    /// Keep a livery's bytes under its hash, the oldest going past the cap.
    fn keep(&mut self, hash: String, bytes: Vec<u8>) {
        if self.kept.contains_key(&hash) {
            return;
        }
        self.bytes += bytes.len();
        self.kept.insert(hash.clone(), Arc::new(bytes));
        self.order.push_back(hash);
        while self.bytes > MAX_STORE_BYTES && self.order.len() > 1 {
            if let Some(old) = self.order.pop_front() {
                if let Some(gone) = self.kept.remove(&old) {
                    self.bytes -= gone.len();
                }
            }
        }
    }

    /// A kept livery's bytes.
    pub fn get(&self, hash: &str) -> Option<Arc<Vec<u8>>> {
        self.kept.get(hash).cloned()
    }

    /// Chunk `chunk` of a kept livery and how many it comes in.
    pub fn chunk(&self, hash: &str, chunk: u32) -> Option<(Vec<u8>, u32)> {
        let bytes = self.kept.get(hash)?;
        let chunks = bytes.len().div_ceil(LIVERY_CHUNK_BYTES).max(1) as u32;
        let start = chunk as usize * LIVERY_CHUNK_BYTES;
        if chunk >= chunks {
            return None;
        }
        let end = (start + LIVERY_CHUNK_BYTES).min(bytes.len());
        Some((bytes[start..end].to_vec(), chunks))
    }

    /// Drop a connection's upload under way (it left).
    pub fn forget_connection(&mut self, connection: ConnectionId) {
        self.uploads.remove(&connection);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::livery::LiveryTexture;
    use crate::own_car::RecordBytes;

    fn png(w: u32, h: u32, pad: usize) -> Vec<u8> {
        let mut b = vec![0x89, b'P', b'N', b'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13];
        b.extend_from_slice(b"IHDR");
        b.extend_from_slice(&w.to_be_bytes());
        b.extend_from_slice(&h.to_be_bytes());
        b.resize(24 + pad, 0);
        b
    }

    fn package(pad: usize) -> Vec<u8> {
        rmp_serde::to_vec_named(&LiveryPackage {
            car: uuid::Uuid::from_u128(5).to_string(),
            name: "Mine".into(),
            paint: Some([0.5, 0.0, 0.0]),
            skin: Some(RecordBytes(png(1024, 1024, pad))),
            textures: vec![LiveryTexture {
                slot: "EXT_RIM".into(),
                image: RecordBytes(png(4, 4, 0)),
            }],
            ..Default::default()
        })
        .unwrap()
    }

    fn upload(store: &mut LiveryStore, conn: u128, bytes: &[u8]) -> Result<Received, Refused> {
        let conn = uuid::Uuid::from_u128(conn);
        let hash = hash_of(bytes);
        let chunks: Vec<&[u8]> = bytes.chunks(LIVERY_CHUNK_BYTES).collect();
        let mut last = Ok(Received::Partial);
        for (i, c) in chunks.iter().enumerate() {
            last = store.receive(conn, &hash, i as u32, chunks.len() as u32, c);
            if last.is_err() {
                break;
            }
        }
        last
    }

    #[test]
    fn a_livery_in_chunks_is_kept_by_its_hash_and_served_back() {
        let mut store = LiveryStore::default();
        let bytes = package(600_000);
        let hash = hash_of(&bytes);
        assert!(
            matches!(upload(&mut store, 1, &bytes), Ok(Received::Done { hash: h, .. }) if h == hash)
        );
        let mut back = Vec::new();
        let (first, chunks) = store.chunk(&hash, 0).unwrap();
        assert_eq!(chunks, 3);
        back.extend(first);
        for c in 1..chunks {
            back.extend(store.chunk(&hash, c).unwrap().0);
        }
        assert_eq!(back, bytes);
        assert_eq!(store.chunk(&hash, chunks), None);
        assert_eq!(store.chunk(&"0".repeat(64), 0), None);
    }

    #[test]
    fn bad_uploads_are_refused() {
        let mut store = LiveryStore::default();
        let one = uuid::Uuid::from_u128(1);
        let bytes = package(10);
        // A wrong hash.
        let refused = store
            .receive(one, &"0".repeat(64), 0, 1, &bytes)
            .unwrap_err();
        assert_eq!(refused.code, 400);
        // Out of order.
        let refused = store
            .receive(one, &hash_of(&bytes), 1, 2, &bytes)
            .unwrap_err();
        assert_eq!(refused.code, 400);
        // Too many chunks, a chunk too big.
        assert_eq!(store.receive(one, "x", 0, 5, &[]).unwrap_err().code, 413);
        let big = vec![0u8; LIVERY_CHUNK_BYTES + 1];
        assert_eq!(store.receive(one, "x", 0, 1, &big).unwrap_err().code, 413);
        // Over the cap in all (four full chunks and one byte more is refused
        // at the chunk count; a package within four chunks but over the cap
        // by its own check).
        let over = package(MAX_LIVERY_BYTES);
        let refused = upload(&mut store, 1, &over).unwrap_err();
        assert_eq!(refused.code, 413);
        // Not an image.
        let mut not_png = rmp_serde::from_slice::<LiveryPackage>(&bytes).unwrap();
        not_png.skin = Some(RecordBytes(b"GIF89a".to_vec()));
        let not_png = rmp_serde::to_vec_named(&not_png).unwrap();
        assert_eq!(upload(&mut store, 1, &not_png).unwrap_err().code, 400);
        assert!(store.get(&hash_of(&not_png)).is_none());
    }

    #[test]
    fn the_oldest_go_past_the_cap() {
        let mut store = LiveryStore::default();
        let mut hashes = Vec::new();
        for i in 0..80 {
            let bytes = package(1_000_000 - 200 + i);
            hashes.push(hash_of(&bytes));
            upload(&mut store, 1, &bytes).unwrap();
        }
        assert!(store.bytes <= MAX_STORE_BYTES);
        assert!(store.get(&hashes[0]).is_none(), "the first is gone");
        assert!(store.get(&hashes[79]).is_some(), "the last is kept");
    }
}
