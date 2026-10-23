//! Build a content pack (`<folder>-<version>.apxpack`) of one circuit or
//! car from this checkout: the files a release ships of it, a manifest
//! with their hashes, its content checksum and the format levels this
//! build writes ([`apexsim_formats::pack`]). Run from `apexsim-content/`
//! after the item's bake (the refresh order of docs/track-pipeline.md).
//!
//! ```text
//! ats-pack track SpaFrankenchamps          # build/packs/SpaFrankenchamps-2026.10.1.apxpack
//! ats-pack car posh-gt3rs --out D:/packs
//! ats-pack track Zandervoort --force       # a new version even if nothing changed
//! ```
//!
//! The version is `<year>.<month>.<n>`, `n` counting this month's packs of
//! the item already in the output folder; a pack whose files are those of
//! the latest one there is not built again.

use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use apexsim_formats::pack::{self, archive, PackKind, PackManifest};

const USAGE: &str = "\
usage: ats-pack (track <Stem> | car <folder>) [--out DIR] [--force]

  --out DIR     where the pack goes (default build/packs)
  --force       build a new version even when nothing changed";

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let mut out = PathBuf::from("build/packs");
    let mut force = false;
    let mut positional = Vec::new();
    let mut i = 0;
    while i < args.len() {
        match args[i].as_str() {
            "--out" if i + 1 < args.len() => {
                out = PathBuf::from(&args[i + 1]);
                i += 1;
            }
            "--force" => force = true,
            "--help" | "-h" => {
                println!("{USAGE}");
                return ExitCode::SUCCESS;
            }
            other if other.starts_with('-') => {
                eprintln!("unknown option {other}\n\n{USAGE}");
                return ExitCode::FAILURE;
            }
            other => positional.push(other.to_string()),
        }
        i += 1;
    }
    let (kind, folder) = match positional.as_slice() {
        [k, f] if k == "track" => (PackKind::Track, f.clone()),
        [k, f] if k == "car" => (PackKind::Car, f.clone()),
        _ => {
            eprintln!("{USAGE}");
            return ExitCode::FAILURE;
        }
    };
    match build(kind, &folder, &out, force) {
        Ok(Some(path)) => {
            println!("{}", path.display());
            ExitCode::SUCCESS
        }
        Ok(None) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("{folder}: {e}");
            ExitCode::FAILURE
        }
    }
}

type Entries = Vec<(String, Vec<u8>)>;

fn add(entries: &mut Entries, name: String, path: &Path) -> Result<(), String> {
    let bytes = std::fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
    entries.push((name, bytes));
    Ok(())
}

fn add_tree(entries: &mut Entries, prefix: &str, dir: &Path) -> Result<(), String> {
    let mut found: Vec<PathBuf> = Vec::new();
    let mut stack = vec![dir.to_path_buf()];
    while let Some(d) = stack.pop() {
        for entry in std::fs::read_dir(&d).map_err(|e| format!("{}: {e}", d.display()))? {
            let path = entry.map_err(|e| e.to_string())?.path();
            if path.is_dir() {
                stack.push(path);
            } else {
                found.push(path);
            }
        }
    }
    found.sort();
    for path in found {
        let rel = path
            .strip_prefix(dir)
            .unwrap()
            .to_string_lossy()
            .replace('\\', "/");
        add(entries, format!("{prefix}/{rel}"), &path)?;
    }
    Ok(())
}

/// A circuit's files: its server folder as a release ships it (the YAML,
/// the sidecars, the guide's and the dossier's files) and its export.
fn track_files(stem: &str) -> Result<(PackManifest, Entries), String> {
    let dir = Path::new("tracks/default").join(stem);
    let yaml = dir.join(format!("{stem}.yaml"));
    let source = std::fs::read(&yaml).map_err(|e| format!("{}: {e}", yaml.display()))?;
    let track: apexsim_formats::track::TrackFile =
        serde_yaml::from_slice(&source).map_err(|e| format!("{}: {e}", yaml.display()))?;
    let id = track
        .track_id
        .clone()
        .ok_or_else(|| format!("{} names no track_id", yaml.display()))?;
    let mut entries = Entries::new();
    let server = format!("server/tracks/default/{stem}");
    add(&mut entries, format!("{server}/{stem}.yaml"), &yaml)?;
    for (suffix, needed) in [
        (".ground.msgpack", true),
        (".curbs.msgpack", true),
        (".walls.msgpack", true),
        (".road.msgpack", true),
        (".pit.msgpack", false),
        (".guide.yml", false),
        (".layout.json", false),
    ] {
        let path = dir.join(format!("{stem}{suffix}"));
        if path.is_file() {
            add(&mut entries, format!("{server}/{stem}{suffix}"), &path)?;
        } else if needed {
            return Err(format!("no {} (bake the circuit first)", path.display()));
        }
    }
    let export = Path::new("build/tracks");
    for suffix in [".uescene.json", ".uemesh"] {
        let path = export.join(format!("{stem}{suffix}"));
        if !path.is_file() {
            return Err(format!("no {} (run ats-export)", path.display()));
        }
        add(&mut entries, format!("game/Tracks/{stem}{suffix}"), &path)?;
    }
    let preview = export.join("previews").join(format!("{stem}.png"));
    if preview.is_file() {
        add(&mut entries, format!("game/Tracks/{stem}.png"), &preview)?;
    }
    let textures = export.join(format!("{stem}.textures"));
    if textures.is_dir() {
        add_tree(
            &mut entries,
            &format!("game/Tracks/{stem}.textures"),
            &textures,
        )?;
    }
    let manifest = manifest(
        PackKind::Track,
        id,
        stem,
        track
            .display_name
            .clone()
            .unwrap_or_else(|| track.name.clone()),
        &source,
    );
    Ok((manifest, entries))
}

/// What a `car.toml` names, by the release's rules (tools/lib/ApexCars.ps1
/// `Get-ApexCarFiles`): its own files, and the class wheels it uses.
fn car_names(toml: &str) -> (Vec<String>, Vec<String>, BTreeMap<String, String>) {
    let mut own = Vec::new();
    let mut wheels = Vec::new();
    let mut top = BTreeMap::new();
    let mut table = String::new();
    for raw in toml.lines() {
        let line = raw.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        if line.starts_with('[') {
            table = line
                .trim_matches(|c| c == '[' || c == ']' || c == ' ')
                .to_string();
            continue;
        }
        if table == "livery" && line.starts_with("textures") {
            for pair in line.split('"').skip(1).step_by(2) {
                if let Some((_, file)) = pair.split_once('=') {
                    own.push(file.trim().to_string());
                }
            }
            continue;
        }
        let Some((key, value)) = line.split_once('=') else {
            continue;
        };
        let (key, value) = (key.trim(), value.trim());
        if !(value.starts_with('"') && value.len() >= 2) {
            continue;
        }
        let value = value[1..].split('"').next().unwrap_or("").to_string();
        match (table.as_str(), key) {
            ("", "model")
            | ("drs_flap", "model")
            | ("cockpit", "steering_wheel_model")
            | ("driver", "model") => own.push(value),
            ("livery", "logo") | ("livery", "skin") | ("livery", "preview") => own.push(value),
            ("wheels", "model") | ("wheels", "rear_model") => {
                if value.ends_with(".glb") || value.contains('/') || value.contains('\\') {
                    own.push(value);
                } else {
                    wheels.push(value);
                }
            }
            ("", k) => {
                top.insert(k.to_string(), value);
            }
            _ => {}
        }
    }
    own.retain(|f| !f.is_empty());
    (own, wheels, top)
}

fn car_files(folder: &str) -> Result<(PackManifest, Entries), String> {
    let dir = Path::new("cars/default").join(folder);
    let toml_path = dir.join("car.toml");
    let source = std::fs::read(&toml_path).map_err(|e| format!("{}: {e}", toml_path.display()))?;
    let text = String::from_utf8_lossy(&source);
    let (own, wheels, top) = car_names(&text);
    let id = top.get("id").cloned().ok_or("car.toml names no id")?;
    let mut entries = Entries::new();
    add(
        &mut entries,
        format!("server/cars/default/{folder}/car.toml"),
        &toml_path,
    )?;
    add(
        &mut entries,
        format!("game/Cars/default/{folder}/car.toml"),
        &toml_path,
    )?;
    for relative in own {
        let relative = relative.replace('\\', "/");
        let path = dir.join(&relative);
        if !path.is_file() {
            return Err(format!("car.toml names {relative}, which is not there"));
        }
        if !entries
            .iter()
            .any(|(n, _)| n == &format!("game/Cars/default/{folder}/{relative}"))
        {
            add(
                &mut entries,
                format!("game/Cars/default/{folder}/{relative}"),
                &path,
            )?;
        }
    }
    for wheel in wheels {
        let path = Path::new("wheels").join(format!("{wheel}.glb"));
        if !path.is_file() {
            return Err(format!("no {} for the {wheel} wheel", path.display()));
        }
        add(&mut entries, format!("game/Wheels/{wheel}.glb"), &path)?;
        for ext in ["glb", "json"] {
            let steering = Path::new("wheels/steering").join(format!("{wheel}.{ext}"));
            if steering.is_file() {
                add(
                    &mut entries,
                    format!("game/Wheels/steering/{wheel}.{ext}"),
                    &steering,
                )?;
            }
        }
    }
    let display = top
        .get("name")
        .cloned()
        .unwrap_or_else(|| folder.to_string());
    Ok((
        manifest(PackKind::Car, id, folder, display, &source),
        entries,
    ))
}

fn manifest(
    kind: PackKind,
    id: String,
    folder: &str,
    display_name: String,
    source: &[u8],
) -> PackManifest {
    let mut built_from = BTreeMap::from([("repo".to_string(), "apexsim-content".to_string())]);
    if let Ok(out) = std::process::Command::new("git")
        .args(["rev-parse", "--short", "HEAD"])
        .output()
    {
        let commit = String::from_utf8_lossy(&out.stdout).trim().to_string();
        if !commit.is_empty() {
            built_from.insert("commit".into(), commit);
        }
    }
    PackManifest {
        pack_format: pack::PACK_FORMAT,
        kind,
        id,
        folder: folder.to_string(),
        display_name,
        version: String::new(),
        content_crc: apexsim_formats::content_crc::content_crc(source),
        formats: pack::written_levels(kind),
        requires: BTreeMap::new(),
        built_from,
        files: BTreeMap::new(),
    }
}

/// Today's year and month, from the clock (civil from days since 1970).
fn year_month() -> (i64, u32) {
    let days = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs() as i64 / 86_400)
        .unwrap_or(0);
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let month = if mp < 10 { mp + 3 } else { mp - 9 } as u32;
    let year = yoe + era * 400 + if month <= 2 { 1 } else { 0 };
    (year, month)
}

/// The packs of `folder` in `out`, by version, newest last.
fn existing(out: &Path, folder: &str) -> Vec<(Vec<u32>, PathBuf)> {
    let prefix = format!("{folder}-");
    let mut found = Vec::new();
    if let Ok(dir) = std::fs::read_dir(out) {
        for entry in dir.flatten() {
            let name = entry.file_name().to_string_lossy().to_string();
            let Some(version) = name
                .strip_prefix(&prefix)
                .and_then(|r| r.strip_suffix(&format!(".{}", pack::PACK_EXTENSION)))
            else {
                continue;
            };
            let parts: Option<Vec<u32>> = version.split('.').map(|p| p.parse().ok()).collect();
            if let Some(parts) = parts.filter(|p| p.len() == 3) {
                found.push((parts, entry.path()));
            }
        }
    }
    found.sort();
    found
}

fn build(kind: PackKind, folder: &str, out: &Path, force: bool) -> Result<Option<PathBuf>, String> {
    let (mut manifest, entries) = match kind {
        PackKind::Track => track_files(folder)?,
        PackKind::Car => car_files(folder)?,
    };
    std::fs::create_dir_all(out).map_err(|e| format!("{}: {e}", out.display()))?;
    let packs = existing(out, folder);
    if let Some((_, latest)) = packs.last() {
        if !force {
            let latest_files = archive::read_manifest(latest)
                .map_err(|e| e.to_string())?
                .files;
            let files: BTreeMap<String, String> = entries
                .iter()
                .map(|(n, b)| (n.clone(), archive::sha256(b)))
                .collect();
            if latest_files == files {
                println!("unchanged since {}", latest.display());
                return Ok(None);
            }
        }
    }
    let (year, month) = year_month();
    let n = packs
        .iter()
        .filter(|(v, _)| v[0] as i64 == year && v[1] == month)
        .map(|(v, _)| v[2])
        .max()
        .unwrap_or(0)
        + 1;
    manifest.version = format!("{year}.{month}.{n}");
    let path = out.join(manifest.file_name());
    archive::write(&path, manifest, &entries).map_err(|e| e.to_string())?;
    Ok(Some(path))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_car_toml_names_its_files_and_wheels() {
        let toml = r#"
id = "abc"
name = "Posh GT3 RS"
model = "posh.glb"
[wheels]
model = "gt3_18"
rear_model = "rims/rear.glb"
[[livery]]
logo = "textures/logo.png"
textures = ["Body=textures/body.png", "Glass=textures/glass.png"]
[cockpit]
steering_wheel_model = "wheel.glb"
"#;
        let (own, wheels, top) = car_names(toml);
        assert_eq!(top.get("id").map(String::as_str), Some("abc"));
        assert_eq!(wheels, vec!["gt3_18".to_string()]);
        for f in [
            "posh.glb",
            "rims/rear.glb",
            "textures/logo.png",
            "textures/body.png",
            "textures/glass.png",
            "wheel.glb",
        ] {
            assert!(own.contains(&f.to_string()), "{f}");
        }
    }
}
