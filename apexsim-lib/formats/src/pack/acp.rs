//! Loose content packs (`<name>.acp`, "ApexSim content pack"): a plain zip
//! of a player's own content, made by hand, for sharing a car, a track or a
//! HUD component. Unlike an `.apxpack` it has no manifest and no hashes; what
//! it holds is worked out from the files, and it installs into the
//! `custom/` folders, never over shipped content.
//!
//! What counts as an item, wherever it sits in the zip:
//!
//! - a **car**: a folder holding `car.toml`; the whole folder. Its name is
//!   the folder's, or the pack's when `car.toml` is at the top.
//! - a **HUD component**: a folder holding `component.json`; the whole
//!   folder, named as a car is.
//! - a **track**: `<Stem>.yaml` with `<Stem>.uescene.json` somewhere in the
//!   zip. Its server half is every `<Stem>.*` file beside the YAML (the
//!   sidecars, `.ats`, the guide and layout); its export (the game's half) is
//!   `<Stem>.uescene.json`, `.uemesh`, `.png` and `<Stem>.textures/`, found
//!   by name anywhere in the zip.
//!
//! A file in no item is reported as ignored. A file in two folder items
//! (a component inside a car folder) belongs to the deeper one.
//!
//! [`read`] keeps a file over [`KEEP_IN_MEMORY`] in the zip ([`Payload::InPack`])
//! and [`Acp::write_to`] copies it out when it is installed, so a pack of
//! track meshes is not held in memory whole.

use std::collections::BTreeSet;

/// A loose pack file's extension.
pub const ACP_EXTENSION: &str = "acp";

/// [`read`] holds a file of this size or less in memory; a larger one (a
/// track's mesh, a car's model) stays in the zip until it is written.
pub const KEEP_IN_MEMORY: u64 = 1 << 20;

/// A file of a loose pack.
#[derive(Debug, Clone, PartialEq)]
pub enum Payload {
    /// Its bytes.
    Bytes(Vec<u8>),
    /// Left in the pack [`Acp::source`] names: its entry and its size.
    InPack { index: usize, size: u64 },
}

impl Payload {
    /// Its size, bytes.
    pub fn len(&self) -> u64 {
        match self {
            Payload::Bytes(b) => b.len() as u64,
            Payload::InPack { size, .. } => *size,
        }
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
}

impl From<Vec<u8>> for Payload {
    fn from(bytes: Vec<u8>) -> Self {
        Payload::Bytes(bytes)
    }
}

/// What an item in a loose pack is.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum AcpKind {
    Car,
    Track,
    Hud,
}

impl AcpKind {
    pub fn label(self) -> &'static str {
        match self {
            AcpKind::Car => "car",
            AcpKind::Track => "track",
            AcpKind::Hud => "hud",
        }
    }
}

/// One item of a loose pack.
#[derive(Debug, Clone, PartialEq)]
pub struct AcpItem {
    pub kind: AcpKind,
    /// The car folder, the track stem or the HUD component id.
    pub name: String,
    /// The item's own folder, each path relative to it: a car's or HUD
    /// component's folder, a track's server folder.
    pub files: Vec<(String, Payload)>,
    /// A track's export, relative to the folder the game reads tracks from
    /// (`<Stem>.uemesh`, `<Stem>.textures/road.png`); empty for the others.
    pub export: Vec<(String, Payload)>,
    /// Why this item cannot be installed (a name that is not a plain
    /// folder name, a track without its mesh); the rest of the pack can.
    pub problem: Option<String>,
    /// What is missing but does not stop the install (a track without one
    /// of its sidecars).
    pub warnings: Vec<String>,
}

/// What a loose pack holds.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct Acp {
    /// By kind, then name.
    pub items: Vec<AcpItem>,
    /// Files in no item, by their path in the zip.
    pub ignored: Vec<String>,
    /// The pack [`read`] read, which holds every [`Payload::InPack`].
    pub source: Option<std::path::PathBuf>,
}

#[cfg(feature = "archive")]
impl Acp {
    fn open(&self) -> Result<zip::ZipArchive<std::fs::File>, AcpError> {
        let path = self
            .source
            .as_ref()
            .ok_or_else(|| AcpError("a file left in a pack that was not read from one".into()))?;
        let fail = |e: &dyn std::fmt::Display| AcpError(format!("{}: {e}", path.display()));
        let file = std::fs::File::open(path).map_err(|e| fail(&e))?;
        zip::ZipArchive::new(file).map_err(|e| fail(&e))
    }

    /// A file's bytes, read out of the pack if it was left there.
    pub fn bytes<'a>(&self, payload: &'a Payload) -> Result<std::borrow::Cow<'a, [u8]>, AcpError> {
        use std::io::Read;
        match payload {
            Payload::Bytes(b) => Ok(std::borrow::Cow::Borrowed(b)),
            Payload::InPack { index, size } => {
                let mut zip = self.open()?;
                let mut entry = zip.by_index(*index).map_err(|e| AcpError(e.to_string()))?;
                let mut out = Vec::with_capacity((*size).min(1 << 30) as usize);
                entry
                    .read_to_end(&mut out)
                    .map_err(|e| AcpError(e.to_string()))?;
                Ok(std::borrow::Cow::Owned(out))
            }
        }
    }

    /// Write a file to `to`, its folders made first; a file left in the
    /// pack is copied out of it as it unpacks, never held whole.
    pub fn write_to(&self, payload: &Payload, to: &std::path::Path) -> Result<(), AcpError> {
        let fail = |e: &dyn std::fmt::Display| AcpError(format!("{}: {e}", to.display()));
        if let Some(dir) = to.parent() {
            std::fs::create_dir_all(dir).map_err(|e| fail(&e))?;
        }
        match payload {
            Payload::Bytes(b) => std::fs::write(to, b).map_err(|e| fail(&e)),
            Payload::InPack { index, .. } => {
                let mut zip = self.open()?;
                let mut entry = zip.by_index(*index).map_err(|e| fail(&e))?;
                let mut out = std::fs::File::create(to).map_err(|e| fail(&e))?;
                std::io::copy(&mut entry, &mut out).map_err(|e| fail(&e))?;
                Ok(())
            }
        }
    }
}

/// Why a loose pack cannot be installed.
#[derive(Debug, Clone, PartialEq)]
pub struct AcpError(pub String);

impl std::fmt::Display for AcpError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for AcpError {}

/// The sidecars a baked track carries beside its YAML.
const TRACK_SIDECARS: [&str; 5] = ["ground", "curbs", "walls", "road", "pit"];

/// Files an archiver adds that are nobody's content.
fn is_junk(path: &str) -> bool {
    path.split('/').any(|part| {
        part == "__MACOSX"
            || part.eq_ignore_ascii_case(".DS_Store")
            || part.eq_ignore_ascii_case("Thumbs.db")
            || part.eq_ignore_ascii_case("desktop.ini")
    })
}

/// A zip entry's name as a relative path with forward slashes, or `None`
/// when it is absolute, names a drive or climbs out with `..`.
pub fn normalise_entry(name: &str) -> Option<String> {
    let path = name.replace('\\', "/");
    if path.starts_with('/') || path.contains(':') {
        return None;
    }
    let parts: Vec<&str> = path
        .split('/')
        .filter(|p| !p.is_empty() && *p != ".")
        .collect();
    if parts.is_empty() || parts.contains(&"..") {
        return None;
    }
    Some(parts.join("/"))
}

/// Whether `name` may name an item folder: letters, digits, `-` and `_`,
/// not starting with `_` (the listings skip those).
pub fn valid_name(name: &str) -> bool {
    !name.is_empty()
        && !name.starts_with('_')
        && name
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || c == '-' || c == '_')
}

/// A pack's file stem as an item name: anything but letters, digits, `-`
/// and `_` becomes `-` (`My Car v2` -> `My-Car-v2`).
fn name_from_stem(stem: &str) -> String {
    let mapped: String = stem
        .chars()
        .map(|c| {
            if c.is_ascii_alphanumeric() || c == '-' || c == '_' {
                c
            } else {
                '-'
            }
        })
        .collect();
    mapped.trim_matches(|c| c == '-' || c == '_').to_string()
}

fn split_dir(path: &str) -> (&str, &str) {
    match path.rfind('/') {
        Some(i) => (&path[..i], &path[i + 1..]),
        None => ("", path),
    }
}

/// `path` relative to folder `dir` ("" is the top), or `None` when outside it.
fn under<'a>(path: &'a str, dir: &str) -> Option<&'a str> {
    if dir.is_empty() {
        Some(path)
    } else {
        path.strip_prefix(dir)
            .and_then(|rest| rest.strip_prefix('/'))
    }
}

/// Where the file at `path` goes in the export folder when it is part of
/// track `stem`'s export, or `None`.
fn export_rest<'a>(path: &'a str, stem: &str) -> Option<&'a str> {
    // `<Stem>.textures/...` may sit in a folder of its own.
    let marker = format!("{stem}.textures/");
    if let Some(i) = path.find(&marker) {
        if i == 0 || path.as_bytes()[i - 1] == b'/' {
            return Some(&path[i..]);
        }
    }
    let (_, file) = split_dir(path);
    let rest = file.strip_prefix(stem)?;
    matches!(rest, ".uescene.json" | ".uemesh" | ".png").then_some(file)
}

/// Sort a loose pack's files into its items. `pack_stem` (the file name
/// without `.acp`) names an item whose marker is at the top of the zip.
pub fn classify(entries: Vec<(String, Payload)>, pack_stem: &str) -> Result<Acp, AcpError> {
    let mut files: Vec<(String, Payload)> = Vec::with_capacity(entries.len());
    let mut seen = BTreeSet::new();
    for (name, bytes) in entries {
        let Some(path) = normalise_entry(&name) else {
            return Err(AcpError(format!("{name}: not a path a pack may hold")));
        };
        if is_junk(&path) {
            continue;
        }
        if !seen.insert(path.clone()) {
            return Err(AcpError(format!("{path}: in the pack twice")));
        }
        files.push((path, bytes));
    }

    // The folder items: (kind, folder in the zip).
    let mut roots: Vec<(AcpKind, String)> = Vec::new();
    for (path, _) in &files {
        let (dir, file) = split_dir(path);
        let kind = if file.eq_ignore_ascii_case("car.toml") {
            AcpKind::Car
        } else if file.eq_ignore_ascii_case("component.json") {
            AcpKind::Hud
        } else {
            continue;
        };
        roots.push((kind, dir.to_string()));
    }
    // The tracks: (stem, folder of the YAML).
    let mut tracks: Vec<(String, String)> = Vec::new();
    for (path, _) in &files {
        let (dir, file) = split_dir(path);
        let Some(stem) = file.strip_suffix(".yaml") else {
            continue;
        };
        let scene = format!("{stem}.uescene.json");
        if files.iter().any(|(p, _)| split_dir(p).1 == scene) {
            tracks.push((stem.to_string(), dir.to_string()));
        }
    }

    let mut acp = Acp::default();
    let mut items: Vec<AcpItem> = Vec::new();
    for (stem, _) in &tracks {
        items.push(AcpItem {
            kind: AcpKind::Track,
            name: stem.clone(),
            files: Vec::new(),
            export: Vec::new(),
            problem: None,
            warnings: Vec::new(),
        });
    }
    for (kind, dir) in &roots {
        let name = match split_dir(dir).1 {
            "" => name_from_stem(pack_stem),
            folder => folder.to_string(),
        };
        items.push(AcpItem {
            kind: *kind,
            name,
            files: Vec::new(),
            export: Vec::new(),
            problem: None,
            warnings: Vec::new(),
        });
    }

    'file: for (path, bytes) in files {
        // A track's files first: they are found by name, not by folder.
        for (i, (stem, dir)) in tracks.iter().enumerate() {
            if let Some(rest) = export_rest(&path, stem) {
                items[i].export.push((rest.to_string(), bytes));
                continue 'file;
            }
            let (file_dir, file) = split_dir(&path);
            if file_dir == dir && file.starts_with(&format!("{stem}.")) {
                items[i].files.push((file.to_string(), bytes));
                continue 'file;
            }
        }
        // Then the deepest folder item holding it.
        let owner = roots
            .iter()
            .enumerate()
            .filter_map(|(i, (_, dir))| under(&path, dir).map(|rest| (i, dir.len(), rest)))
            .max_by_key(|(_, depth, _)| *depth);
        match owner {
            Some((i, _, rest)) => {
                let rest = rest.to_string();
                items[tracks.len() + i].files.push((rest, bytes));
            }
            None => acp.ignored.push(path),
        }
    }

    for item in &mut items {
        if !valid_name(&item.name) {
            item.problem = Some(format!(
                "{:?} is not a folder name of letters, digits, - and _ only",
                item.name
            ));
            continue;
        }
        if item.kind != AcpKind::Track {
            continue;
        }
        let stem = &item.name;
        if !item
            .export
            .iter()
            .any(|(p, _)| *p == format!("{stem}.uemesh"))
        {
            item.problem = Some(format!("no {stem}.uemesh beside its {stem}.uescene.json"));
        }
        for sidecar in TRACK_SIDECARS {
            let name = format!("{stem}.{sidecar}.msgpack");
            if !item.files.iter().any(|(p, _)| *p == name) {
                item.warnings.push(format!("no {name}"));
            }
        }
    }
    items.sort_by(|a, b| (a.kind, &a.name).cmp(&(b.kind, &b.name)));
    for pair in items.windows(2) {
        if pair[0].kind == pair[1].kind && pair[0].name.eq_ignore_ascii_case(&pair[1].name) {
            return Err(AcpError(format!(
                "two {}s named {} in the pack",
                pair[0].kind.label(),
                pair[0].name
            )));
        }
    }
    if items.is_empty() {
        return Err(AcpError(
            "the pack holds no car (a folder with car.toml), no track (<Stem>.yaml with \
             <Stem>.uescene.json) and no HUD component (a folder with component.json)"
                .into(),
        ));
    }
    acp.items = items;
    Ok(acp)
}

/// Read and sort a loose pack.
#[cfg(feature = "archive")]
pub fn read(path: &std::path::Path) -> Result<Acp, AcpError> {
    use std::io::Read;
    let fail = |e: &dyn std::fmt::Display| AcpError(format!("{}: {e}", path.display()));
    let file = std::fs::File::open(path).map_err(|e| fail(&e))?;
    let mut zip = zip::ZipArchive::new(file).map_err(|e| fail(&e))?;
    let mut entries = Vec::with_capacity(zip.len());
    for i in 0..zip.len() {
        let mut entry = zip.by_index(i).map_err(|e| fail(&e))?;
        if entry.is_dir() {
            continue;
        }
        let name = entry.name().to_string();
        let size = entry.size();
        if size > KEEP_IN_MEMORY {
            entries.push((name, Payload::InPack { index: i, size }));
            continue;
        }
        let mut bytes = Vec::with_capacity(size as usize);
        entry.read_to_end(&mut bytes).map_err(|e| fail(&e))?;
        entries.push((name, Payload::Bytes(bytes)));
    }
    let stem = path
        .file_stem()
        .map(|s| s.to_string_lossy().into_owned())
        .unwrap_or_default();
    let mut acp = classify(entries, &stem)?;
    acp.source = Some(path.to_path_buf());
    Ok(acp)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn entries(names: &[&str]) -> Vec<(String, Payload)> {
        names
            .iter()
            .map(|n| (n.to_string(), Payload::Bytes(n.as_bytes().to_vec())))
            .collect()
    }

    fn names(files: &[(String, Payload)]) -> Vec<&str> {
        let mut v: Vec<&str> = files.iter().map(|(p, _)| p.as_str()).collect();
        v.sort();
        v
    }

    #[test]
    fn a_car_folder_is_a_car_wherever_it_sits() {
        let acp = classify(
            entries(&[
                "my-gt3/car.toml",
                "my-gt3/body.glb",
                "my-gt3/liveries/red.png",
                "README.txt",
                "__MACOSX/my-gt3/._car.toml",
            ]),
            "pack",
        )
        .unwrap();
        assert_eq!(acp.items.len(), 1);
        let car = &acp.items[0];
        assert_eq!((car.kind, car.name.as_str()), (AcpKind::Car, "my-gt3"));
        assert_eq!(
            names(&car.files),
            ["body.glb", "car.toml", "liveries/red.png"]
        );
        assert_eq!(acp.ignored, ["README.txt"]);
    }

    #[test]
    fn a_car_at_the_top_is_named_after_the_pack() {
        let acp = classify(entries(&["car.toml", "body.glb"]), "My Car v2").unwrap();
        assert_eq!(acp.items[0].name, "My-Car-v2");
        assert_eq!(names(&acp.items[0].files), ["body.glb", "car.toml"]);
    }

    #[test]
    fn hud_components_and_a_car_share_a_pack_and_the_deeper_folder_wins() {
        let acp = classify(
            entries(&[
                "hud/my_relative/component.json",
                "hud/my_gauge/component.json",
                "hud/my_gauge/needle.png",
                "cars/mine/car.toml",
                "cars/mine/extras/component.json",
            ]),
            "pack",
        )
        .unwrap();
        let got: Vec<(AcpKind, &str, usize)> = acp
            .items
            .iter()
            .map(|i| (i.kind, i.name.as_str(), i.files.len()))
            .collect();
        assert_eq!(
            got,
            [
                (AcpKind::Car, "mine", 1),
                (AcpKind::Hud, "extras", 1),
                (AcpKind::Hud, "my_gauge", 2),
                (AcpKind::Hud, "my_relative", 1),
            ]
        );
    }

    #[test]
    fn a_track_is_its_yaml_folder_and_its_export_found_by_name() {
        let acp = classify(
            entries(&[
                "server/Ring/Ring.yaml",
                "server/Ring/Ring.ground.msgpack",
                "server/Ring/Ring.curbs.msgpack",
                "server/Ring/Ring.walls.msgpack",
                "server/Ring/Ring.road.msgpack",
                "server/Ring/Ring.pit.msgpack",
                "server/Ring/Ring.ats",
                "server/Ring/notes.md",
                "game/Ring.uescene.json",
                "game/Ring.uemesh",
                "game/Ring.png",
                "game/Ring.textures/road.png",
            ]),
            "pack",
        )
        .unwrap();
        let track = &acp.items[0];
        assert_eq!((track.kind, track.name.as_str()), (AcpKind::Track, "Ring"));
        assert_eq!(
            names(&track.files),
            [
                "Ring.ats",
                "Ring.curbs.msgpack",
                "Ring.ground.msgpack",
                "Ring.pit.msgpack",
                "Ring.road.msgpack",
                "Ring.walls.msgpack",
                "Ring.yaml"
            ]
        );
        assert_eq!(
            names(&track.export),
            [
                "Ring.png",
                "Ring.textures/road.png",
                "Ring.uemesh",
                "Ring.uescene.json"
            ]
        );
        assert_eq!(acp.ignored, ["server/Ring/notes.md"]);
        assert!(track.warnings.is_empty() && track.problem.is_none());
    }

    #[test]
    fn a_track_in_one_folder_without_sidecars_installs_with_warnings() {
        let acp = classify(
            entries(&["Ring.yaml", "Ring.uescene.json", "Ring.uemesh"]),
            "pack",
        )
        .unwrap();
        assert_eq!(names(&acp.items[0].files), ["Ring.yaml"]);
        assert_eq!(
            names(&acp.items[0].export),
            ["Ring.uemesh", "Ring.uescene.json"]
        );
        assert_eq!(acp.items[0].warnings.len(), TRACK_SIDECARS.len());
    }

    #[test]
    fn a_yaml_without_an_export_is_not_a_track() {
        let err = classify(entries(&["Ring/Ring.yaml"]), "pack").unwrap_err();
        assert!(err.0.contains("holds no car"), "{err}");
    }

    #[test]
    fn a_track_without_its_mesh_is_an_item_that_cannot_be_installed() {
        let acp = classify(
            entries(&["Ring.yaml", "Ring.uescene.json", "car/car.toml"]),
            "pack",
        )
        .unwrap();
        assert!(acp.items[0].problem.is_none(), "the car is fine");
        let problem = acp.items[1].problem.as_deref().unwrap();
        assert!(problem.contains("Ring.uemesh"), "{problem}");
    }

    #[test]
    fn a_path_that_climbs_out_is_refused() {
        for bad in [
            "../car.toml",
            "/abs/car.toml",
            "C:/car.toml",
            "a/../../car.toml",
        ] {
            assert!(classify(entries(&[bad]), "pack").is_err(), "{bad}");
        }
        assert_eq!(
            normalise_entry("a\\b\\car.toml").as_deref(),
            Some("a/b/car.toml")
        );
    }

    #[test]
    fn a_folder_name_that_is_not_a_plain_name_cannot_be_installed() {
        let acp = classify(entries(&["my car/car.toml"]), "pack").unwrap();
        let problem = acp.items[0].problem.as_deref().unwrap();
        assert!(problem.contains("folder name"), "{problem}");
    }

    #[test]
    fn two_items_of_one_kind_and_name_are_refused() {
        let err = classify(entries(&["a/x/car.toml", "b/x/car.toml"]), "pack").unwrap_err();
        assert!(err.0.contains("two cars named x"), "{err}");
    }

    /// A file over `KEEP_IN_MEMORY` stays in the zip as it is read and
    /// comes out of it whole when written.
    #[cfg(feature = "archive")]
    #[test]
    fn a_large_file_stays_in_the_pack_until_it_is_written() {
        use std::io::Write;
        let dir = std::env::temp_dir().join(format!("apexsim-acp-large-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        let pack = dir.join("big.acp");
        let mesh: Vec<u8> = (0..(KEEP_IN_MEMORY as usize + 4096))
            .map(|i| (i * 31 % 251) as u8)
            .collect();
        {
            let mut zip = zip::ZipWriter::new(std::fs::File::create(&pack).unwrap());
            let opts = zip::write::SimpleFileOptions::default();
            zip.start_file("my-gt3/car.toml", opts).unwrap();
            zip.write_all(
                b"id = \"x\"
",
            )
            .unwrap();
            zip.start_file("my-gt3/body.glb", opts).unwrap();
            zip.write_all(&mesh).unwrap();
            zip.finish().unwrap();
        }
        let acp = read(&pack).unwrap();
        let car = &acp.items[0];
        let body = &car.files.iter().find(|(p, _)| p == "body.glb").unwrap().1;
        assert!(matches!(body, Payload::InPack { .. }), "{body:?}");
        assert_eq!(body.len(), mesh.len() as u64);
        let toml = &car.files.iter().find(|(p, _)| p == "car.toml").unwrap().1;
        assert!(matches!(toml, Payload::Bytes(_)));
        let out = dir.join("out/body.glb");
        acp.write_to(body, &out).unwrap();
        assert_eq!(std::fs::read(&out).unwrap(), mesh);
        assert_eq!(acp.bytes(body).unwrap().as_ref(), mesh.as_slice());
        let _ = std::fs::remove_dir_all(&dir);
    }
}
