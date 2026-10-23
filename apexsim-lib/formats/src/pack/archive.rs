//! Reading and writing a pack's zip (feature `archive`): the one place a
//! pack's bytes are made and checked, for the content's builder
//! (`ats-pack`) and the code's installer (`apexsim-server content`).

use super::{safe_entry, PackManifest, MANIFEST_NAME};
use sha2::{Digest, Sha256};
use std::io::{Read, Write};
use std::path::Path;

/// What went wrong with a pack.
#[derive(Debug)]
pub enum PackError {
    Io(std::io::Error),
    Zip(String),
    /// The manifest is missing or does not parse.
    Manifest(String),
    /// A file is not what the manifest says, or not where a pack may write.
    Content(String),
}

impl std::fmt::Display for PackError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            PackError::Io(e) => write!(f, "{e}"),
            PackError::Zip(e) => write!(f, "not a readable pack: {e}"),
            PackError::Manifest(e) => write!(f, "pack.json: {e}"),
            PackError::Content(e) => write!(f, "{e}"),
        }
    }
}

impl std::error::Error for PackError {}

impl From<std::io::Error> for PackError {
    fn from(e: std::io::Error) -> Self {
        PackError::Io(e)
    }
}

impl From<zip::result::ZipError> for PackError {
    fn from(e: zip::result::ZipError) -> Self {
        PackError::Zip(e.to_string())
    }
}

/// A pack's files: each path in the pack with its bytes.
pub type PackFiles = Vec<(String, Vec<u8>)>;

/// `sha256:<hex>` of some bytes, as the manifest names its files.
pub fn sha256(bytes: &[u8]) -> String {
    let digest = Sha256::digest(bytes);
    let mut out = String::with_capacity(7 + 64);
    out.push_str("sha256:");
    for b in digest {
        out.push_str(&format!("{b:02x}"));
    }
    out
}

/// Write a pack: `manifest` (its `files` filled here from `entries`) and
/// each `(path in the pack, bytes)`. The manifest goes first, so a reader
/// that only wants it stops there.
pub fn write(
    path: &Path,
    mut manifest: PackManifest,
    entries: &[(String, Vec<u8>)],
) -> Result<PackManifest, PackError> {
    manifest.files.clear();
    for (name, bytes) in entries {
        if !manifest.may_hold(name) {
            return Err(PackError::Content(format!(
                "{name}: not a path this pack may hold"
            )));
        }
        manifest.files.insert(name.clone(), sha256(bytes));
    }
    let file = std::fs::File::create(path)?;
    let mut zip = zip::ZipWriter::new(file);
    let options = zip::write::SimpleFileOptions::default()
        .compression_method(zip::CompressionMethod::Deflated)
        // A fixed time, so the same files make the same pack.
        .last_modified_time(zip::DateTime::default());
    zip.start_file(MANIFEST_NAME, options)?;
    let json =
        serde_json::to_vec_pretty(&manifest).map_err(|e| PackError::Manifest(e.to_string()))?;
    zip.write_all(&json)?;
    let mut sorted: Vec<&(String, Vec<u8>)> = entries.iter().collect();
    sorted.sort_by(|a, b| a.0.cmp(&b.0));
    for (name, bytes) in sorted {
        zip.start_file(name.as_str(), options)?;
        zip.write_all(bytes)?;
    }
    zip.finish()?;
    Ok(manifest)
}

fn manifest_of<R: Read + std::io::Seek>(
    zip: &mut zip::ZipArchive<R>,
) -> Result<PackManifest, PackError> {
    let mut text = String::new();
    zip.by_name(MANIFEST_NAME)
        .map_err(|_| PackError::Manifest("missing".into()))?
        .read_to_string(&mut text)?;
    serde_json::from_str(&text).map_err(|e| PackError::Manifest(e.to_string()))
}

/// A pack's manifest alone.
pub fn read_manifest(path: &Path) -> Result<PackManifest, PackError> {
    let mut zip = zip::ZipArchive::new(std::fs::File::open(path)?)?;
    manifest_of(&mut zip)
}

/// A pack's manifest and every file, each checked against the manifest's
/// hash and against where a pack of its kind may write. Nothing else is
/// in the zip, and nothing the manifest names is missing.
pub fn read(path: &Path) -> Result<(PackManifest, PackFiles), PackError> {
    let mut zip = zip::ZipArchive::new(std::fs::File::open(path)?)?;
    let manifest = manifest_of(&mut zip)?;
    let mut files = Vec::new();
    for i in 0..zip.len() {
        let mut entry = zip.by_index(i)?;
        let name = entry.name().to_string();
        if name == MANIFEST_NAME || entry.is_dir() {
            continue;
        }
        if !manifest.may_hold(&name) {
            return Err(PackError::Content(format!(
                "{name}: not a path this pack may hold"
            )));
        }
        let Some(expected) = manifest.files.get(&name) else {
            return Err(PackError::Content(format!("{name}: not in pack.json")));
        };
        let mut bytes = Vec::with_capacity(entry.size() as usize);
        entry.read_to_end(&mut bytes)?;
        if sha256(&bytes) != *expected {
            return Err(PackError::Content(format!(
                "{name}: does not match its hash"
            )));
        }
        files.push((name, bytes));
    }
    for name in manifest.files.keys() {
        if !files.iter().any(|(n, _)| n == name) {
            return Err(PackError::Content(format!(
                "{name}: named in pack.json, not in the pack"
            )));
        }
    }
    debug_assert!(files.iter().all(|(n, _)| safe_entry(n)));
    Ok((manifest, files))
}

#[cfg(test)]
mod tests {
    use super::super::*;
    use super::*;

    fn manifest() -> PackManifest {
        PackManifest {
            pack_format: PACK_FORMAT,
            kind: PackKind::Track,
            id: "id".into(),
            folder: "Spa".into(),
            display_name: "Spa".into(),
            version: "2026.10.1".into(),
            content_crc: 1,
            formats: written_levels(PackKind::Track),
            requires: Default::default(),
            built_from: Default::default(),
            files: Default::default(),
        }
    }

    #[test]
    fn a_pack_reads_back_what_was_written_and_is_the_same_bytes_each_time() {
        let dir = std::env::temp_dir().join(format!("apexsim-pack-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let entries = vec![
            (
                "server/tracks/default/Spa/Spa.yaml".to_string(),
                b"track".to_vec(),
            ),
            ("game/Tracks/Spa.uemesh".to_string(), vec![1, 2, 3]),
        ];
        let a = dir.join("a.apxpack");
        let b = dir.join("b.apxpack");
        let written = write(&a, manifest(), &entries).unwrap();
        write(&b, manifest(), &entries).unwrap();
        assert_eq!(std::fs::read(&a).unwrap(), std::fs::read(&b).unwrap());
        assert_eq!(written.files.len(), 2);
        let (m, files) = read(&a).unwrap();
        assert_eq!(m, written);
        assert_eq!(files.len(), 2);
        assert_eq!(read_manifest(&a).unwrap().folder, "Spa");
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn a_pack_may_not_write_outside_its_item() {
        let dir = std::env::temp_dir().join(format!("apexsim-pack-bad-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        for bad in [
            "server/tracks/default/Monza/Monza.yaml",
            "server/cars/default/Spa/car.toml",
            "game/Tracks/Monza.uemesh",
            "game/ApexSim.exe",
        ] {
            let err = write(
                &dir.join("x.apxpack"),
                manifest(),
                &[(bad.to_string(), vec![0])],
            );
            assert!(err.is_err(), "{bad}");
        }
        let _ = std::fs::remove_dir_all(&dir);
    }
}
