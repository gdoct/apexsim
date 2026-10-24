//! Content packs: one car or circuit as a zip (`<id>-<version>.apxpack`)
//! with a manifest, `pack.json` ([`PackManifest`]), and the install's
//! record of what packs it holds, `Content/installed.json` ([`Installed`]).
//!
//! A pack states the format levels its files are written in
//! ([`PackManifest::formats`]); a reader states the levels it reads
//! ([`Reads`]) and refuses a pack outside them before touching the install
//! ([`Reads::refuses`]). A pack never names a code version
//! (`apexsim/docs/proposals/split-off-content.md`, "Version rules").
//!
//! The zip holds `pack.json`, `server/...` (what a server's
//! `content/<kind>/default/<folder>/` holds) and `game/...` (what a packaged
//! game's `Tracks/` or `Cars/default/<folder>/` and `Wheels/` hold); every file but
//! the manifest is named with its SHA-256 in [`PackManifest::files`].

use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;

pub mod acp;
#[cfg(feature = "archive")]
pub mod archive;

/// The level of `pack.json` itself.
pub const PACK_FORMAT: u32 = 1;
/// The manifest's name inside a pack.
pub const MANIFEST_NAME: &str = "pack.json";
/// A pack file's extension.
pub const PACK_EXTENSION: &str = "apxpack";

/// What a pack holds.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum PackKind {
    Track,
    Car,
}

impl PackKind {
    /// The folder under a server's `content/` (`tracks`, `cars`).
    pub fn content_dir(self) -> &'static str {
        match self {
            PackKind::Track => "tracks",
            PackKind::Car => "cars",
        }
    }
}

/// The format levels a pack's files are written in, by format name:
/// `track_yaml`, `sidecars` and `scene` for a circuit, `car_toml` for a car.
pub type FormatLevels = BTreeMap<String, u32>;

/// `pack.json`.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct PackManifest {
    /// [`PACK_FORMAT`] of the manifest.
    pub pack_format: u32,
    pub kind: PackKind,
    /// The track's `track_id` or the car's `id`: the key records, catalog
    /// rows and showcases use. A pack never changes it.
    pub id: String,
    /// The folder the item lives in: a track's stem, a car's folder name.
    pub folder: String,
    pub display_name: String,
    /// `<year>.<month>.<n>`: what people read.
    pub version: String,
    /// [`crate::content_crc`] of the YAML or `car.toml`: what the two
    /// sides compare at runtime.
    pub content_crc: u32,
    pub formats: FormatLevels,
    /// Content on content (`props` for the props a circuit was dressed
    /// against); empty when none.
    #[serde(default, skip_serializing_if = "BTreeMap::is_empty")]
    pub requires: BTreeMap<String, String>,
    /// Where it was built from (`repo`, `commit`); display only.
    #[serde(default, skip_serializing_if = "BTreeMap::is_empty")]
    pub built_from: BTreeMap<String, String>,
    /// Every file of the pack but this manifest, by its path in the zip
    /// (`server/Spa.yaml`, `game/Spa.uemesh`), with `sha256:<hex>`.
    pub files: BTreeMap<String, String>,
}

impl PackManifest {
    /// The file name a pack of this manifest is written under.
    pub fn file_name(&self) -> String {
        format!("{}-{}.{}", self.folder, self.version, PACK_EXTENSION)
    }

    /// Whether this pack may hold (and so an install may write) `path`:
    /// only its own item's files. A circuit's are its server folder
    /// (`server/tracks/default/<Stem>/`) and its export beside the game
    /// (`game/Tracks/<Stem>.*`, `game/Tracks/<Stem>.textures/`); a car's its
    /// server folder, its game folder (`game/Cars/default/<folder>/`) and the class
    /// wheels it names (`game/Wheels/`).
    pub fn may_hold(&self, path: &str) -> bool {
        if !safe_entry(path) || self.folder.is_empty() || self.folder.contains(['/', '\\', '.']) {
            return false;
        }
        let server = format!(
            "server/{}/default/{}/",
            self.kind.content_dir(),
            self.folder
        );
        if path.starts_with(&server) {
            return true;
        }
        match self.kind {
            PackKind::Track => {
                let export = format!("game/Tracks/{}.", self.folder);
                path.strip_prefix(&export)
                    .is_some_and(|rest| !rest.contains('/') || rest.starts_with("textures/"))
            }
            PackKind::Car => {
                path.starts_with(&format!("game/Cars/default/{}/", self.folder))
                    || path.starts_with("game/Wheels/")
            }
        }
    }
}

/// One installed pack, as `installed.json` lists it.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct InstalledPack {
    pub kind: PackKind,
    pub id: String,
    pub folder: String,
    pub display_name: String,
    pub version: String,
    pub content_crc: u32,
    /// The pack file kept under `Content/packs/` for repair and roll back.
    pub pack_file: String,
    /// RFC 3339; display only.
    #[serde(default)]
    pub installed_at: String,
}

/// `Content/installed.json`: which packs an install holds. Only reported,
/// never what decides what is loaded: the server loads what is under its
/// content folders, the game what is beside it.
#[derive(Debug, Clone, Default, PartialEq, Serialize, Deserialize)]
pub struct Installed {
    pub version: u32,
    pub packs: Vec<InstalledPack>,
}

impl Installed {
    /// Record `pack` (replacing an earlier version of the same item).
    pub fn put(&mut self, pack: InstalledPack) {
        self.packs
            .retain(|p| !(p.kind == pack.kind && p.id == pack.id));
        self.packs.push(pack);
        self.packs
            .sort_by(|a, b| (a.kind as u8, &a.folder).cmp(&(b.kind as u8, &b.folder)));
        self.version = 1;
    }

    pub fn find(&self, kind: PackKind, id: &str) -> Option<&InstalledPack> {
        self.packs
            .iter()
            .find(|p| p.kind == kind && p.id.eq_ignore_ascii_case(id))
    }
}

/// The levels of each format a build reads, inclusive.
#[derive(Debug, Clone, PartialEq)]
pub struct Reads(pub BTreeMap<&'static str, (u32, u32)>);

impl Reads {
    /// What this build of the formats crate reads: every format from level
    /// 1 to the level it writes.
    pub fn current() -> Self {
        Reads(BTreeMap::from([
            ("track_yaml", (1, crate::TRACK_YAML_LEVEL)),
            ("sidecars", (1, crate::SIDECARS_LEVEL)),
            ("scene", (1, crate::SCENE_LEVEL)),
            ("car_toml", (1, crate::CAR_TOML_LEVEL)),
        ]))
    }

    /// Why a pack written at `formats` cannot be read here, naming the
    /// format, or `None` when it can. A format this build has never heard
    /// of is refused too: it is newer than the build.
    pub fn refuses(&self, formats: &FormatLevels) -> Option<String> {
        for (name, level) in formats {
            match self.0.get(name.as_str()) {
                None => return Some(format!("{name} is a format this build does not know")),
                Some((low, high)) if level < low || level > high => {
                    return Some(format!(
                        "{name} level {level} (this build reads {low} to {high})"
                    ))
                }
                Some(_) => {}
            }
        }
        None
    }
}

/// The levels a pack of `kind` written by this build states.
pub fn written_levels(kind: PackKind) -> FormatLevels {
    match kind {
        PackKind::Track => FormatLevels::from([
            ("track_yaml".to_string(), crate::TRACK_YAML_LEVEL),
            ("sidecars".to_string(), crate::SIDECARS_LEVEL),
            ("scene".to_string(), crate::SCENE_LEVEL),
        ]),
        PackKind::Car => FormatLevels::from([("car_toml".to_string(), crate::CAR_TOML_LEVEL)]),
    }
}

/// Whether a path inside a pack is one an install may write: relative,
/// forward slashes, no `..`, no drive or root, under `server/` or `game/`.
pub fn safe_entry(path: &str) -> bool {
    (path.starts_with("server/") || path.starts_with("game/"))
        && !path.contains('\\')
        && !path.contains(':')
        && path.split('/').all(|part| !part.is_empty() && part != "..")
}

#[cfg(test)]
mod tests {
    use super::*;

    fn manifest() -> PackManifest {
        PackManifest {
            pack_format: PACK_FORMAT,
            kind: PackKind::Track,
            id: "b8edab00-8392-4765-967e-3c060828f922".into(),
            folder: "SpaFrankenchamps".into(),
            display_name: "Spa".into(),
            version: "2026.10.1".into(),
            content_crc: 42,
            formats: written_levels(PackKind::Track),
            requires: BTreeMap::new(),
            built_from: BTreeMap::new(),
            files: BTreeMap::from([("server/SpaFrankenchamps.yaml".into(), "sha256:00".into())]),
        }
    }

    #[test]
    fn a_manifest_reads_back_and_names_its_file() {
        let m = manifest();
        let json = serde_json::to_string(&m).unwrap();
        assert!(json.contains("\"kind\":\"track\""));
        assert!(!json.contains("requires"), "left out when empty");
        assert_eq!(serde_json::from_str::<PackManifest>(&json).unwrap(), m);
        assert_eq!(m.file_name(), "SpaFrankenchamps-2026.10.1.apxpack");
    }

    #[test]
    fn a_reader_refuses_a_level_outside_its_range_by_name() {
        let reads = Reads::current();
        assert_eq!(reads.refuses(&written_levels(PackKind::Track)), None);
        assert_eq!(reads.refuses(&written_levels(PackKind::Car)), None);
        let newer = FormatLevels::from([("scene".to_string(), crate::SCENE_LEVEL + 1)]);
        assert!(reads.refuses(&newer).unwrap().starts_with("scene level"));
        let unknown = FormatLevels::from([("hologram".to_string(), 1)]);
        assert!(reads.refuses(&unknown).unwrap().contains("hologram"));
    }

    #[test]
    fn installing_again_replaces_the_items_entry() {
        let mut installed = Installed::default();
        let entry = |version: &str| InstalledPack {
            kind: PackKind::Track,
            id: "id-1".into(),
            folder: "Spa".into(),
            display_name: "Spa".into(),
            version: version.into(),
            content_crc: 1,
            pack_file: format!("Spa-{version}.apxpack"),
            installed_at: String::new(),
        };
        installed.put(entry("2026.10.1"));
        installed.put(entry("2026.10.2"));
        assert_eq!(installed.packs.len(), 1);
        assert_eq!(
            installed.find(PackKind::Track, "ID-1").unwrap().version,
            "2026.10.2"
        );
    }

    #[test]
    fn only_paths_under_server_or_game_may_be_written() {
        assert!(safe_entry("server/Spa.yaml"));
        assert!(safe_entry("game/textures/logo.png"));
        assert!(!safe_entry("server/../../evil.dll"));
        assert!(!safe_entry("/etc/passwd"));
        assert!(!safe_entry("C:/x"));
        assert!(!safe_entry("server\\x"));
        assert!(!safe_entry("other/x"));
        assert!(!safe_entry("server//x"));
    }
}
