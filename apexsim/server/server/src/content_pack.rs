//! Installing content packs into a release install (`apexsim-server
//! content`): one circuit's or car's `.apxpack` ([`apexsim_formats::pack`])
//! written where the server and the game already look, with
//! `Content/installed.json` recording which packs the install holds.
//!
//! ```text
//! <install>/
//!   Server/content/<tracks|cars>/default/<folder>/   the pack's server/...
//!   Game/Tracks/<Stem>.*, Game/Cars/default/<folder>/, Game/Wheels/   its game/...
//!   Content/installed.json, Content/packs/<file>.apxpack
//! ```
//!
//! A pack is read whole and checked (every file against its hash, every
//! path inside its own item, the format levels against what this build
//! reads) before the install is touched. Nothing here runs in a live
//! server: what is loaded stays what is on disk under the content folders.

use apexsim_formats::pack::{self, archive, Installed, InstalledPack, PackKind, PackManifest};
use std::path::{Path, PathBuf};

/// The install a server binary sits in: `<root>/Server/apexsim-server.exe`
/// with `<root>/Game` beside it; `explicit` when given.
pub fn install_root(explicit: Option<PathBuf>) -> Result<PathBuf, String> {
    if let Some(root) = explicit {
        return if root.join("Server").is_dir() && root.join("Game").is_dir() {
            Ok(root)
        } else {
            Err(format!(
                "{} holds no Server and Game folders",
                root.display()
            ))
        };
    }
    let exe = std::env::current_exe().map_err(|e| e.to_string())?;
    exe.parent()
        .and_then(Path::parent)
        .filter(|root| root.join("Server").is_dir() && root.join("Game").is_dir())
        .map(Path::to_path_buf)
        .ok_or_else(|| "not in a release install: pass --root <install folder>".to_string())
}

fn installed_path(root: &Path) -> PathBuf {
    root.join("Content").join("installed.json")
}

/// `Content/installed.json`, empty when there is none yet.
pub fn read_installed(root: &Path) -> Result<Installed, String> {
    match std::fs::read(installed_path(root)) {
        Ok(bytes) => serde_json::from_slice(&bytes).map_err(|e| format!("installed.json: {e}")),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(Installed::default()),
        Err(e) => Err(format!("installed.json: {e}")),
    }
}

/// Where a pack's file goes in an install.
fn target(root: &Path, name: &str) -> Option<PathBuf> {
    if let Some(rest) = name.strip_prefix("server/") {
        Some(root.join("Server").join("content").join(rest))
    } else {
        name.strip_prefix("game/")
            .map(|rest| root.join("Game").join(rest))
    }
}

fn write_file(path: &Path, bytes: &[u8]) -> std::io::Result<()> {
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir)?;
    }
    let tmp = path.with_extension("apxtmp");
    std::fs::write(&tmp, bytes)?;
    std::fs::rename(&tmp, path)
}

/// The folders an item owns whole: emptied before its files are written,
/// so a file the new version dropped does not linger.
fn owned_dirs(root: &Path, manifest: &PackManifest) -> Vec<PathBuf> {
    let server = root
        .join("Server")
        .join("content")
        .join(manifest.kind.content_dir())
        .join("default")
        .join(&manifest.folder);
    match manifest.kind {
        PackKind::Track => vec![
            server,
            root.join("Game")
                .join("Tracks")
                .join(format!("{}.textures", manifest.folder)),
        ],
        PackKind::Car => vec![
            server,
            root.join("Game")
                .join("Cars")
                .join("default")
                .join(&manifest.folder),
        ],
    }
}

/// Install `pack_path` into `root`: checked whole first, then its files
/// written, the pack kept under `Content/packs/` and recorded in
/// `installed.json`. Returns what was recorded.
pub fn install(root: &Path, pack_path: &Path) -> Result<InstalledPack, String> {
    let (manifest, files) = archive::read(pack_path).map_err(|e| e.to_string())?;
    if manifest.pack_format != pack::PACK_FORMAT {
        return Err(format!(
            "pack format {} (this build reads {})",
            manifest.pack_format,
            pack::PACK_FORMAT
        ));
    }
    if let Some(why) = pack::Reads::current().refuses(&manifest.formats) {
        return Err(format!("this build cannot read the pack: {why}"));
    }
    for dir in owned_dirs(root, &manifest) {
        if dir.is_dir() {
            std::fs::remove_dir_all(&dir).map_err(|e| format!("{}: {e}", dir.display()))?;
        }
    }
    for (name, bytes) in &files {
        let path = target(root, name).ok_or_else(|| format!("{name}: nowhere to go"))?;
        write_file(&path, bytes).map_err(|e| format!("{}: {e}", path.display()))?;
    }
    let packs = root.join("Content").join("packs");
    std::fs::create_dir_all(&packs).map_err(|e| format!("{}: {e}", packs.display()))?;
    let kept = packs.join(manifest.file_name());
    if std::fs::canonicalize(pack_path).ok() != std::fs::canonicalize(&kept).ok() {
        std::fs::copy(pack_path, &kept).map_err(|e| format!("{}: {e}", kept.display()))?;
    }
    let entry = InstalledPack {
        kind: manifest.kind,
        id: manifest.id.clone(),
        folder: manifest.folder.clone(),
        display_name: manifest.display_name.clone(),
        version: manifest.version.clone(),
        content_crc: manifest.content_crc,
        pack_file: manifest.file_name(),
        installed_at: crate::records::now_rfc3339(),
    };
    let mut installed = read_installed(root)?;
    installed.put(entry.clone());
    let json = serde_json::to_vec_pretty(&installed).map_err(|e| e.to_string())?;
    write_file(&installed_path(root), &json).map_err(|e| format!("installed.json: {e}"))?;
    Ok(entry)
}

/// Each installed pack's files on disk against its kept pack: the names of
/// the files missing or changed, per pack (empty when it is intact).
pub fn verify(root: &Path) -> Result<Vec<(InstalledPack, Vec<String>)>, String> {
    let installed = read_installed(root)?;
    let mut out = Vec::new();
    for entry in installed.packs {
        let kept = root.join("Content").join("packs").join(&entry.pack_file);
        let problems = match archive::read_manifest(&kept) {
            Err(e) => vec![format!("{}: {e}", kept.display())],
            Ok(manifest) => manifest
                .files
                .iter()
                .filter_map(|(name, hash)| {
                    let path = target(root, name)?;
                    match std::fs::read(&path) {
                        Ok(bytes) if archive::sha256(&bytes) == *hash => None,
                        Ok(_) => Some(format!("{name} changed")),
                        Err(_) => Some(format!("{name} missing")),
                    }
                })
                .collect(),
        };
        out.push((entry, problems));
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::BTreeMap;

    fn release() -> PathBuf {
        let root = std::env::temp_dir().join(format!("apexsim-install-{}", uuid::Uuid::new_v4()));
        std::fs::create_dir_all(root.join("Server/content/tracks/default")).unwrap();
        std::fs::create_dir_all(root.join("Game/Tracks")).unwrap();
        root
    }

    fn spa(version: &str, yaml: &[u8], dir: &Path) -> PathBuf {
        let manifest = PackManifest {
            pack_format: pack::PACK_FORMAT,
            kind: PackKind::Track,
            id: "spa-id".into(),
            folder: "Spa".into(),
            display_name: "Spa".into(),
            version: version.into(),
            content_crc: apexsim_formats::content_crc::content_crc(yaml),
            formats: pack::written_levels(PackKind::Track),
            requires: BTreeMap::new(),
            built_from: BTreeMap::new(),
            files: BTreeMap::new(),
        };
        let path = dir.join(manifest.file_name());
        archive::write(
            &path,
            manifest,
            &[
                ("server/tracks/default/Spa/Spa.yaml".into(), yaml.to_vec()),
                ("game/Tracks/Spa.uemesh".into(), vec![7; 16]),
            ],
        )
        .unwrap();
        path
    }

    #[test]
    fn a_pack_installs_where_both_sides_look_and_a_new_version_replaces_it() {
        let root = release();
        let built = root.join("built");
        std::fs::create_dir_all(&built).unwrap();
        // A file the old version had and the new one does not.
        std::fs::create_dir_all(root.join("Server/content/tracks/default/Spa")).unwrap();
        std::fs::write(
            root.join("Server/content/tracks/default/Spa/old.msgpack"),
            b"x",
        )
        .unwrap();

        let entry = install(&root, &spa("2026.10.1", b"name: Spa", &built)).unwrap();
        assert_eq!(entry.version, "2026.10.1");
        assert_eq!(
            std::fs::read(root.join("Server/content/tracks/default/Spa/Spa.yaml")).unwrap(),
            b"name: Spa"
        );
        assert!(root.join("Game/Tracks/Spa.uemesh").is_file());
        assert!(!root
            .join("Server/content/tracks/default/Spa/old.msgpack")
            .exists());
        assert!(root.join("Content/packs/Spa-2026.10.1.apxpack").is_file());

        install(&root, &spa("2026.10.2", b"name: Spa 2", &built)).unwrap();
        let installed = read_installed(&root).unwrap();
        assert_eq!(installed.packs.len(), 1);
        assert_eq!(installed.packs[0].version, "2026.10.2");
        assert!(verify(&root).unwrap()[0].1.is_empty(), "intact");

        // A hand edit shows in the check.
        std::fs::write(root.join("Game/Tracks/Spa.uemesh"), b"edited").unwrap();
        assert_eq!(
            verify(&root).unwrap()[0].1,
            vec!["game/Tracks/Spa.uemesh changed".to_string()]
        );
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn a_pack_this_build_cannot_read_is_refused_before_anything_is_written() {
        let root = release();
        let mut manifest = archive::read_manifest(&spa("2026.10.1", b"x", &root)).unwrap();
        manifest
            .formats
            .insert("scene".into(), apexsim_formats::SCENE_LEVEL + 1);
        manifest.version = "2026.10.9".into();
        let path = root.join(manifest.file_name());
        archive::write(
            &path,
            manifest,
            &[("game/Tracks/Spa.uemesh".into(), vec![1])],
        )
        .unwrap();
        let err = install(&root, &path).unwrap_err();
        assert!(err.contains("scene level"), "{err}");
        assert!(!root.join("Game/Tracks/Spa.uemesh").exists());
        assert!(!installed_path(&root).exists());
        let _ = std::fs::remove_dir_all(&root);
    }
}
